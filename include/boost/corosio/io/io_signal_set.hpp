//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_IO_SIGNAL_SET_HPP
#define BOOST_COROSIO_IO_IO_SIGNAL_SET_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/io_env.hpp>

#include <coroutine>
#include <stop_token>
#include <system_error>

namespace boost::corosio {

/** Abstract base for asynchronous signal sets.

    Provides the common signal set interface: `wait` and `cancel`.
    Concrete classes like @ref signal_set add signal registration
    (add, remove, clear) and platform-specific flags.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Unsafe.

    @see signal_set, io_object
*/
class BOOST_COROSIO_DECL io_signal_set : public io_object
{
    struct wait_awaitable
    {
        io_signal_set& s_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable int signal_number_ = 0;

        explicit wait_awaitable(io_signal_set& s) noexcept : s_(s) {}

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
            return s_.get().wait(
                h, env->executor, token_, &ec_, &signal_number_);
        }
    };

public:
    /** Define backend hooks for signal set wait and cancel.

        Platform backends derive from this to implement
        signal delivery notification.
    */
    struct implementation : io_object::implementation
    {
        /** Initiate an asynchronous wait for a signal.

            @param h Coroutine handle to resume on completion.
            @param ex Executor for dispatching the completion.
            @param token Stop token for cancellation.
            @param ec Output error code.
            @param signo Output signal number.

            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> wait(
            std::coroutine_handle<> h,
            capy::executor_ref ex,
            std::stop_token token,
            std::error_code* ec,
            int* signo) = 0;

        /** Cancel all pending wait operations.

            Cancelled waiters complete with an error that
            compares equal to `capy::cond::canceled`.
        */
        virtual void cancel() = 0;
    };

    /** Cancel all operations associated with the signal set.

        Forces the completion of any pending asynchronous wait
        operations. Each cancelled operation completes with an error
        code that compares equal to `capy::cond::canceled`.

        Cancellation does not alter the set of registered signals.
    */
    void cancel()
    {
        do_cancel();
    }

    /** Wait for a signal to be delivered.

        The operation supports cancellation via `std::stop_token` through
        the affine awaitable protocol. If the associated stop token is
        triggered, the operation completes immediately with an error
        that compares equal to `capy::cond::canceled`.

        This signal set must outlive the returned awaitable.

        @return An awaitable that completes with `io_result<int>`.
            Returns the signal number when a signal is delivered,
            or an error code on failure.
    */
    auto wait()
    {
        return wait_awaitable(*this);
    }

protected:
    /** Dispatch cancel to the concrete implementation. */
    virtual void do_cancel() = 0;

    explicit io_signal_set(handle h) noexcept : io_object(std::move(h)) {}

    /// Move construct.
    io_signal_set(io_signal_set&& other) noexcept : io_object(std::move(other))
    {
    }

    /// Move assign.
    io_signal_set& operator=(io_signal_set&& other) noexcept
    {
        if (this != &other)
            h_ = std::move(other.h_);
        return *this;
    }

    io_signal_set(io_signal_set const&)            = delete;
    io_signal_set& operator=(io_signal_set const&) = delete;

private:
    implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif
