//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_URING_URING_SOCKET_SERVICE_BASE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_URING_URING_SOCKET_SERVICE_BASE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_URING

#include <boost/corosio/detail/object_pool.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/corosio/native/detail/uring/uring_scheduler.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <vector>

/*
    Shared lifecycle plumbing for io_uring socket/datagram services.

    construct / destroy / shutdown / close / scheduler() are identical across
    uring_tcp_service, uring_udp_service, uring_local_stream_service,
    and uring_local_datagram_service — they all pop-or-new the impl from a
    per-service recycling pool, cancel on shutdown, and close eagerly. This
    base factors that out; the concrete services add only the protocol-
    specific open/bind/adopt.

    This is io_uring's own service base rather than a reuse of
    reactor_socket_service: io_uring cancels (not closes) on shutdown and
    constructs impls with a (service&, scheduler&) ctor, vs. the reactor's
    close-on-shutdown and Impl(Derived&) ctor. Reusing the reactor template
    would force a teardown behavior change for marginal extra sharing. See
    tasks/proactor-dedup-decisions.md (#13).

    Requirements on Socket: a `(Derived& service, uring_scheduler& sched)`
    constructor, a `void close_socket() noexcept` method (cancel in-flight
    ops + close fd + reset cached endpoints), a `void reuse() noexcept`
    method, and derivation from `intrusive_list<Socket>::node` (the pool's
    live/free linkage).

    @tparam Derived     The concrete service (CRTP, passed to the Socket ctor).
    @tparam ServiceBase The abstract service vtable base (tcp_service, ...).
    @tparam Socket      The concrete io_uring socket impl type.
*/

namespace boost::corosio::detail {

template<class Derived, class ServiceBase, class Socket>
class uring_socket_service_base : public ServiceBase
{
    friend Derived;
    friend Socket;

    // Private CRTP ctor: only `Derived` (the concrete service, a friend)
    // constructs the base — prevents inheriting with the wrong Derived
    // (bugprone-crtp-constructor-accessibility).
    explicit uring_socket_service_base(capy::execution_context& ctx)
        : sched_(&ctx.template use_service<uring_scheduler>())
    {
    }

public:
    ~uring_socket_service_base() override = default;

    void shutdown() override
    {
        // Snapshot live impls under an acquired reference, then cancel
        // without the pool lock held to avoid inversion if cancel() ever
        // re-enters the service. shutdown() sets shutting-down and
        // takes the snapshot under one critical section, so each
        // cancel's own release (if it drops the last ref) deletes
        // rather than recycles. In-flight ops hold their own
        // references, so every impl stays alive while its cancel CQEs
        // are processed.
        std::vector<Socket*> live;
        pool_.shutdown(
            [&](Socket* s)
            {
                acquire(s);
                live.push_back(s);
            });
        for (auto* s : live)
        {
            s->cancel();
            release(s);
        }
    }

    io_object::implementation* construct() override
    {
        return acquire_impl();
    }

    void destroy(io_object::implementation* p) override
    {
        if (!p)
            return;
        release(static_cast<Socket*>(p));
    }

    // Close the fd eagerly when the public close() is called, before
    // destroy() drops the service's reference and recycling runs.
    void close(io_object::handle& h) override
    {
        if (auto* sock = static_cast<Socket*>(h.get()))
            sock->close_socket();
    }

    /// Return the scheduler used by sockets created by this service.
    uring_scheduler& scheduler() noexcept
    {
        return *sched_;
    }

protected:
    /** Pop a recycled impl or news one, with the service reference held.

        Used by both `construct()` and the `adopt_fd` accepted-connection
        path (which additionally calls `assign_fd()` on the result).
    */
    Socket* acquire_impl()
    {
        return pool_.acquire(static_cast<Derived&>(*this), *sched_);
    }

    uring_scheduler* sched_;
    object_pool<Socket> pool_;

private:
    uring_socket_service_base(uring_socket_service_base const&) = delete;
    uring_socket_service_base&
    operator=(uring_socket_service_base const&) = delete;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_URING_URING_SOCKET_SERVICE_BASE_HPP
