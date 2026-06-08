//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_SOCKET_SERVICE_BASE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_SOCKET_SERVICE_BASE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IO_URING

#include <boost/corosio/io/io_object.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_scheduler.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

/*
    Shared lifecycle plumbing for io_uring socket/datagram services.

    construct / destroy / shutdown / close / scheduler() are identical across
    io_uring_tcp_service, io_uring_udp_service, io_uring_local_stream_service,
    and io_uring_local_datagram_service — they all make_shared the impl, track
    it in a raw->shared_ptr map, cancel on shutdown, and close eagerly. This
    base factors that out; the concrete services add only the protocol-
    specific open/bind/adopt.

    This is io_uring's own service base rather than a reuse of
    reactor_socket_service: io_uring tracks impls in a map (the reactor uses
    an intrusive list + map), cancels (not closes) on shutdown, and constructs
    impls with a (service&, scheduler&) ctor. Reusing the reactor template
    would force io_uring sockets to adopt the intrusive node, a close-on-
    shutdown behavior change, and an Impl(Derived&) ctor — high churn and a
    teardown behavior change for marginal extra sharing. See
    tasks/proactor-dedup-decisions.md (#13).

    Requirements on Socket: a `(Derived& service, io_uring_scheduler& sched)`
    constructor and a `void close_socket() noexcept` method (cancel in-flight
    ops + close fd + reset cached endpoints).

    @tparam Derived     The concrete service (CRTP, passed to the Socket ctor).
    @tparam ServiceBase The abstract service vtable base (tcp_service, ...).
    @tparam Socket      The concrete io_uring socket impl type.
*/

namespace boost::corosio::detail {

template<class Derived, class ServiceBase, class Socket>
class io_uring_socket_service_base : public ServiceBase
{
    friend Derived;

    // Private CRTP ctor: only `Derived` (the concrete service, a friend)
    // constructs the base — prevents inheriting with the wrong Derived
    // (bugprone-crtp-constructor-accessibility).
    explicit io_uring_socket_service_base(capy::execution_context& ctx)
        : sched_(&ctx.template use_service<io_uring_scheduler>())
    {
    }

public:
    ~io_uring_socket_service_base() override = default;

    void shutdown() override
    {
        // Snapshot live impls, then cancel without the lock held to avoid
        // inversion if cancel() ever re-enters the service. Impls stay owned
        // by impls_ until ~service (after the scheduler drains its queue),
        // keeping every impl alive while its cancel CQEs are processed.
        std::vector<std::shared_ptr<Socket>> live;
        {
            std::lock_guard lk(mutex_);
            live.reserve(impls_.size());
            for (auto& [_, p] : impls_)
                live.push_back(p);
        }
        for (auto& p : live)
            p->cancel();
    }

    io_object::implementation* construct() override
    {
        auto  p   = std::make_shared<Socket>(
            static_cast<Derived&>(*this), *sched_);
        auto* raw = p.get();
        std::lock_guard lk(mutex_);
        impls_.emplace(raw, std::move(p));
        return raw;
    }

    void destroy(io_object::implementation* p) override
    {
        if (!p)
            return;
        std::lock_guard lk(mutex_);
        impls_.erase(static_cast<Socket*>(p));
    }

    // Close the fd eagerly when the public close() is called, before
    // destroy() drops the shared_ptr and the destructor runs.
    void close(io_object::handle& h) override
    {
        if (auto* sock = static_cast<Socket*>(h.get()))
            sock->close_socket();
    }

    /// Return the scheduler used by sockets created by this service.
    io_uring_scheduler& scheduler() noexcept { return *sched_; }

protected:
    /// Register an externally-built impl (used by adopt_fd on stream
    /// services after accept(2)). Returns the raw pointer.
    Socket* register_impl(std::shared_ptr<Socket> p)
    {
        auto* raw = p.get();
        std::lock_guard lk(mutex_);
        impls_.emplace(raw, std::move(p));
        return raw;
    }

    io_uring_scheduler* sched_;
    std::mutex          mutex_;
    std::unordered_map<Socket*, std::shared_ptr<Socket>> impls_;

private:
    io_uring_socket_service_base(io_uring_socket_service_base const&) = delete;
    io_uring_socket_service_base&
    operator=(io_uring_socket_service_base const&) = delete;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IO_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_SOCKET_SERVICE_BASE_HPP
