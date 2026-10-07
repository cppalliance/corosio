//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SOCKET_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SOCKET_SERVICE_HPP

#include <boost/corosio/io/io_object.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/corosio/native/detail/reactor/reactor_service_state.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <memory>

namespace boost::corosio::detail {

/** CRTP base for reactor-backed socket/datagram service implementations.

    Provides the shared construct/destroy/shutdown/close/post/work
    logic that is identical across all reactor backends and socket
    types. Derived classes add only protocol-specific open/bind.

    @tparam Derived     The concrete service type (CRTP).
    @tparam ServiceBase The abstract service base (tcp_service
                        or udp_service).
    @tparam Scheduler   The backend's scheduler type.
    @tparam Impl        The backend's socket/datagram impl type.
*/
template<class Derived, class ServiceBase, class Scheduler, class Impl>
class reactor_socket_service : public ServiceBase
{
    friend Derived;

    // The intermediate CRTP templates below (not Impl itself) are what
    // actually reach into state_->pool_ from retire() -- see
    // reactor_socket_finals.hpp.
    template<class, class, class, class, class, class>
    friend class reactor_stream_socket_impl;
    template<class, class, class, class, class, class>
    friend class reactor_dgram_socket_impl;

    using state_type = reactor_service_state<Scheduler, Impl>;

protected:
    // NOLINTNEXTLINE(bugprone-crtp-constructor-accessibility)
    explicit reactor_socket_service(capy::execution_context& ctx)
        : state_(
              std::make_unique<state_type>(
                  ctx.template use_service<Scheduler>()))
    {
    }

public:
    ~reactor_socket_service() override = default;

    void shutdown() override
    {
        state_->pool_.shutdown(
            [this](Impl* impl)
            {
                static_cast<Derived*>(this)->pre_shutdown(impl);
                impl->close_socket();
            });

        // Queued ops still hold references (their own object_ref copy);
        // the scheduler shuts down after us and drains completed_ops_,
        // each op's destroy() releasing its reference. Shutting-down
        // mode makes that final release() delete instead of recycle,
        // so every impl stays alive until its ops are gone, then frees.
    }

    io_object::implementation* construct() override
    {
        return state_->pool_.acquire(static_cast<Derived&>(*this));
    }

    void destroy(io_object::implementation* impl) override
    {
        auto* typed = static_cast<Impl*>(impl);
        static_cast<Derived*>(this)->pre_destroy(typed);
        typed->close_socket();
        release(typed);
    }

    void close(io_object::handle& h) override
    {
        static_cast<Impl*>(h.get())->close_socket();
    }

    Scheduler& scheduler() const noexcept
    {
        return state_->sched_;
    }

    void post(scheduler_op* op)
    {
        state_->sched_.post(op);
    }

    void work_started() noexcept
    {
        state_->sched_.work_started();
    }

    void work_finished() noexcept
    {
        state_->sched_.work_finished();
    }

protected:
    // Override in derived to add pre-close logic. No backend currently needs
    // it; the hooks exist so a trait can run fd-level teardown before close.
    void pre_shutdown(Impl*) noexcept {
    } // LCOV_EXCL_LINE optional CRTP hook; no backend overrides it today
    void pre_destroy(Impl*) noexcept {}

    std::unique_ptr<state_type> state_;

private:
    reactor_socket_service(reactor_socket_service const&)            = delete;
    reactor_socket_service& operator=(reactor_socket_service const&) = delete;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SOCKET_SERVICE_HPP
