//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DESCRIPTOR_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DESCRIPTOR_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/detail/descriptor_service.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/corosio/native/detail/validate_fd.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor.hpp>
#include <boost/corosio/native/detail/reactor/reactor_service_state.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <memory>
#include <mutex>
#include <system_error>

/* Reactor-backed descriptor_service.

   assign_descriptor is the validate-before-mutate core the public
   assign() contract rests on, modelled on do_assign_fd in
   reactor_service_finals.hpp.

   PARALLEL COPY: construct, destroy, close and shutdown here mirror the
   same four members of reactor_socket_service.hpp, which this cannot
   reuse because it calls close_socket() by name. A fix to the service
   lifecycle -- the pool's acquire/recycle, or shutdown's reliance on
   queued ops holding their own reference through the scheduler's
   drain -- belongs in both files.
*/

namespace boost::corosio::detail {

/** CRTP base for reactor-backed descriptor services.

    @tparam Derived   The named final service type (CRTP self).
    @tparam Traits    Backend traits (epoll_traits, kqueue_traits, ...).
    @tparam DescFinal The named final descriptor impl type.
*/
template<class Derived, class Traits, class DescFinal>
class reactor_descriptor_service : public descriptor_service
{
    using scheduler_type = typename Traits::scheduler_type;
    using state_type     = reactor_service_state<scheduler_type, DescFinal>;

    friend Derived;

    // The CRTP base reaches into state_->pool_ from retire().
    template<class, class, class, class>
    friend class reactor_descriptor;

protected:
    // NOLINTNEXTLINE(bugprone-crtp-constructor-accessibility)
    explicit reactor_descriptor_service(capy::execution_context& ctx)
        : state_(
              std::make_unique<state_type>(
                  ctx.template use_service<scheduler_type>()))
    {
    }

public:
    /// True when any parked op must wake the reactor.
    static constexpr bool needs_park_notification =
        Traits::needs_park_notification;

    ~reactor_descriptor_service() override = default;

    std::error_code assign_descriptor(
        posix_stream_descriptor::implementation& impl,
        native_handle_type fd) override;

    void shutdown() override
    {
        state_->pool_.shutdown(
            [](DescFinal* impl) { impl->close_descriptor(); });

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
        auto* typed = static_cast<DescFinal*>(impl);
        typed->close_descriptor();
        release(typed);
    }

    void close(io_object::handle& h) override
    {
        static_cast<DescFinal*>(h.get())->close_descriptor();
    }

    scheduler_type& scheduler() const noexcept
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
    std::unique_ptr<state_type> state_;

private:
    reactor_descriptor_service(reactor_descriptor_service const&) = delete;
    reactor_descriptor_service&
    operator=(reactor_descriptor_service const&) = delete;
};

template<class Derived, class Traits, class DescFinal>
std::error_code
reactor_descriptor_service<Derived, Traits, DescFinal>::assign_descriptor(
    posix_stream_descriptor::implementation& impl_base, native_handle_type fd)
{
    auto* impl = static_cast<DescFinal*>(&impl_base);

    // The public assign() guarantees the object is closed.
    if (auto ec = validate_descriptor_fd(fd))
        return ec;

    if constexpr (requires { Traits::max_descriptor; })
    {
        if (fd >= Traits::max_descriptor)
            return make_err(EMFILE);
    }

    return impl->init_and_register(fd);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DESCRIPTOR_SERVICE_HPP
