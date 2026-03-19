//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_SIGNAL_SET_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_SIGNAL_SET_HPP

#include <boost/corosio/signal_set.hpp>
#include <boost/corosio/backend.hpp>

#ifndef BOOST_COROSIO_MRDOCS
#if BOOST_COROSIO_HAS_EPOLL || BOOST_COROSIO_HAS_SELECT || \
    BOOST_COROSIO_HAS_KQUEUE
#include <boost/corosio/native/detail/posix/posix_signal_service.hpp>
#endif

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_signals.hpp>
#endif
#endif // !BOOST_COROSIO_MRDOCS

namespace boost::corosio {

/** An asynchronous signal set with devirtualized wait operations.

    This class template inherits from @ref signal_set and shadows
    the `wait` operation with a version that calls the backend
    implementation directly, allowing the compiler to inline
    through the entire call chain.

    Non-async operations (`add`, `remove`, `clear`, `cancel`)
    remain unchanged and dispatch through the compiled library.

    A `native_signal_set` IS-A `signal_set` and can be passed to
    any function expecting `signal_set&`.

    @tparam Backend A backend tag value (e.g., `epoll`).

    @par Thread Safety
    Same as @ref signal_set.

    @see signal_set, epoll_t, iocp_t
*/
template<auto Backend>
class native_signal_set : public signal_set
{
    using backend_type = decltype(Backend);
    using impl_type    = typename backend_type::signal_type;

    impl_type& get_impl() noexcept
    {
        return *static_cast<impl_type*>(h_.get());
    }

    struct native_wait_awaitable
    {
        native_signal_set& self_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable int signal_number_ = 0;

        explicit native_wait_awaitable(native_signal_set& self) noexcept
            : self_(self)
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        capy::io_result<int> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {capy::error::canceled, 0};
            return {ec_, signal_number_};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            return self_.get_impl().wait(
                h, env->executor, token_, &ec_, &signal_number_);
        }
    };

public:
    /** Construct a native signal set from an execution context.

        @param ctx The execution context that will own this signal set.
    */
    explicit native_signal_set(capy::execution_context& ctx) : signal_set(ctx)
    {
    }

    /** Construct a native signal set with initial signals.

        @param ctx The execution context that will own this signal set.
        @param signal First signal number to add.
        @param signals Additional signal numbers to add.

        @throws std::system_error on failure.
    */
    template<std::convertible_to<int>... Signals>
    native_signal_set(
        capy::execution_context& ctx, int signal, Signals... signals)
        : signal_set(ctx, signal, signals...)
    {
    }

    /** Move construct.

        @param other The signal set to move from.

        @pre No awaitables returned by @p other's methods exist.
        @pre The execution context associated with @p other must
            outlive this signal set.
    */
    native_signal_set(native_signal_set&&) noexcept = default;

    /** Move assign.

        @param other The signal set to move from.

        @pre No awaitables returned by either `*this` or @p other's
            methods exist.
        @pre The execution context associated with @p other must
            outlive this signal set.
    */
    native_signal_set& operator=(native_signal_set&&) noexcept = default;

    native_signal_set(native_signal_set const&)            = delete;
    native_signal_set& operator=(native_signal_set const&) = delete;

    /** Wait for a signal to be delivered.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref signal_set::wait.

        @return An awaitable yielding `io_result<int>`.

        This signal set must outlive the returned awaitable.
    */
    auto wait()
    {
        return native_wait_awaitable(*this);
    }
};

} // namespace boost::corosio

#endif
