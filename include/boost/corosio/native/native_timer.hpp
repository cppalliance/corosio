//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_TIMER_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_TIMER_HPP

#include <boost/corosio/timer.hpp>
#include <boost/corosio/backend.hpp>
#include <boost/corosio/detail/timer_service.hpp>

namespace boost::corosio {

/** An asynchronous timer with devirtualized wait operations.

    This class template inherits from @ref timer and shadows the
    `wait` operation with a version that calls the backend
    implementation directly, allowing the compiler to inline
    through the entire call chain.

    Non-async operations (`cancel`, `expires_at`, `expires_after`)
    remain unchanged and dispatch through the compiled library.

    A `native_timer` IS-A `timer` and can be passed to any function
    expecting `timer&`.

    @tparam Backend A backend tag value (e.g., `epoll`).
        The timer implementation is backend-independent; the
        tag selects the concrete impl type for devirtualization.

    @par Thread Safety
    Same as @ref timer.

    @see timer, epoll_t, iocp_t
*/
template<auto Backend>
class native_timer : public timer
{
    using impl_type = detail::timer_service::implementation;

    impl_type& get_impl() noexcept
    {
        return *static_cast<impl_type*>(h_.get());
    }

    struct native_wait_awaitable
    {
        native_timer& self_;
        std::stop_token token_;
        mutable std::error_code ec_;
        capy::continuation cont_;

        explicit native_wait_awaitable(native_timer& self) noexcept
            : self_(self)
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        capy::io_result<> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {capy::error::canceled};
            return {ec_};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_     = env->stop_token;
            cont_.h    = h;
            auto& impl = self_.get_impl();
            // Fast path: already expired and not in the heap
            if (impl.heap_index_ == timer::implementation::npos &&
                (impl.expiry_ == (time_point::min)() ||
                 impl.expiry_ <= clock_type::now()))
            {
                ec_    = {};
                auto d = env->executor;
                d.post(cont_);
                return std::noop_coroutine();
            }
            return impl.wait(h, env->executor, std::move(token_), &ec_, &cont_);
        }
    };

public:
    /** Construct a native timer from an execution context.

        @param ctx The execution context that will own this timer.
    */
    explicit native_timer(capy::execution_context& ctx) : timer(ctx) {}

    /** Construct a native timer with an initial absolute expiry.

        @param ctx The execution context that will own this timer.
        @param t The initial expiry time point.
    */
    native_timer(capy::execution_context& ctx, time_point t) : timer(ctx, t) {}

    /** Construct a native timer with an initial relative expiry.

        @param ctx The execution context that will own this timer.
        @param d The initial expiry duration relative to now.
    */
    template<class Rep, class Period>
    native_timer(
        capy::execution_context& ctx, std::chrono::duration<Rep, Period> d)
        : timer(ctx, d)
    {
    }

    /** Move construct.

        @param other The timer to move from.

        @pre No awaitables returned by @p other's methods exist.
        @pre The execution context associated with @p other must
            outlive this timer.
    */
    native_timer(native_timer&&) noexcept = default;

    /** Move assign.

        @param other The timer to move from.

        @pre No awaitables returned by either `*this` or @p other's
            methods exist.
        @pre The execution context associated with @p other must
            outlive this timer.
    */
    native_timer& operator=(native_timer&&) noexcept = default;

    native_timer(native_timer const&)            = delete;
    native_timer& operator=(native_timer const&) = delete;

    /** Wait for the timer to expire.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref timer::wait.

        @return An awaitable yielding `io_result<>`.

        This timer must outlive the returned awaitable.
    */
    auto wait()
    {
        return native_wait_awaitable(*this);
    }
};

} // namespace boost::corosio

#endif
