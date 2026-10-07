//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_ACCEPTOR_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_ACCEPTOR_SERVICE_HPP

#include <boost/corosio/io/io_object.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/corosio/native/detail/reactor/reactor_service_state.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <memory>

namespace boost::corosio::detail {

/* CRTP base for reactor-backed acceptor service implementations.

   Provides the shared construct/destroy/shutdown/close/post/work
   logic that is identical across all reactor backends and acceptor
   types (TCP and local stream). Derived classes add only
   protocol-specific open/bind/listen/stream_service.

   @tparam Derived       The concrete service type (CRTP).
   @tparam ServiceBase   The abstract service base
                         (tcp_acceptor_service or
                          local_stream_acceptor_service).
   @tparam Scheduler     The backend's scheduler type.
   @tparam Impl          The backend's acceptor impl type.
   @tparam StreamService The concrete stream service type returned
                         by stream_service().
*/
template<
    class Derived,
    class ServiceBase,
    class Scheduler,
    class Impl,
    class StreamService>
class reactor_acceptor_service : public ServiceBase
{
    friend Derived;

    // The intermediate CRTP template below (not Impl itself) is what
    // actually reaches into state_->pool_ from retire() -- see
    // reactor_socket_finals.hpp.
    template<class, class, class, class, class, class>
    friend class reactor_acceptor_impl;

    using state_type = reactor_service_state<Scheduler, Impl>;

protected:
    // NOLINTNEXTLINE(bugprone-crtp-constructor-accessibility)
    explicit reactor_acceptor_service(capy::execution_context& ctx)
        : ctx_(ctx)
        , state_(
              std::make_unique<state_type>(
                  ctx.template use_service<Scheduler>()))
    {
    }

public:
    ~reactor_acceptor_service() override = default;

    void shutdown() override
    {
        state_->pool_.shutdown([](Impl* impl) { impl->close_socket(); });

        // See reactor_socket_service::shutdown(): queued ops hold their
        // own reference; the scheduler's drain releases them after us,
        // and shutting-down mode deletes rather than recycles at zero.
    }

    io_object::implementation* construct() override
    {
        return state_->pool_.acquire(static_cast<Derived&>(*this));
    }

    void destroy(io_object::implementation* impl) override
    {
        auto* typed = static_cast<Impl*>(impl);
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

    StreamService* stream_service() const noexcept
    {
        return stream_svc_;
    }

protected:
    capy::execution_context& ctx_;
    std::unique_ptr<state_type> state_;
    StreamService* stream_svc_ = nullptr;

private:
    reactor_acceptor_service(reactor_acceptor_service const&) = delete;
    reactor_acceptor_service&
    operator=(reactor_acceptor_service const&) = delete;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_ACCEPTOR_SERVICE_HPP
