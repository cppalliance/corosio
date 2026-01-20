//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_SIGNAL_SET_HPP
#define BOOST_COROSIO_SIGNAL_SET_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/io_object.hpp>
#include <boost/corosio/io_result.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/any_executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/concept/io_awaitable.hpp>
#include <boost/capy/concept/executor.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>

#include <concepts>
#include <coroutine>
#include <stop_token>

namespace boost {
namespace corosio {

/** An asynchronous signal set for coroutine I/O.

    This class provides the ability to perform an asynchronous wait
    for one or more signals to occur. The signal set registers for
    signals using the C runtime signal() function.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. A signal_set must not have concurrent
    wait operations.

    @par Supported Signals
    On Windows, the following signals are supported:
    SIGINT, SIGTERM, SIGABRT, SIGFPE, SIGILL, SIGSEGV.

    @par Example
    @code
    signal_set signals(ctx, SIGINT, SIGTERM);
    auto [ec, signum] = co_await signals.async_wait();
    if (!ec)
        std::cout << "Received signal " << signum << std::endl;
    @endcode
*/
class BOOST_COROSIO_DECL signal_set : public io_object
{
    struct wait_awaitable
    {
        signal_set& s_;
        std::stop_token token_;
        mutable system::error_code ec_;
        mutable int signal_number_ = 0;

        explicit wait_awaitable(signal_set& s) noexcept : s_(s) {}

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        io_result<int> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {capy::error::canceled};
            return {ec_, signal_number_};
        }

        template<typename Ex>
        auto await_suspend(
            std::coroutine_handle<> h,
            Ex const& ex,
            std::stop_token token) -> std::coroutine_handle<>
        {
            token_ = std::move(token);
            s_.get().wait(h, ex, token_, &ec_, &signal_number_);
            return std::noop_coroutine();
        }
    };

public:
    struct signal_set_impl : io_object_impl
    {
        virtual void wait(
            std::coroutine_handle<>,
            capy::any_executor_ref,
            std::stop_token,
            system::error_code*,
            int*) = 0;

        virtual system::result<void> add(int signal_number) = 0;
        virtual system::result<void> remove(int signal_number) = 0;
        virtual system::result<void> clear() = 0;
        virtual void cancel() = 0;
    };

    /** Destructor.

        Cancels any pending operations and releases signal resources.
    */
    ~signal_set();

    /** Construct an empty signal set.

        @param ctx The execution context that will own this signal set.
    */
    explicit signal_set(capy::execution_context& ctx);

    /** Construct a signal set with initial signals.

        @param ctx The execution context that will own this signal set.
        @param signal First signal number to add.
        @param signals Additional signal numbers to add.

        @throws boost::system::system_error Thrown on failure.
    */
    template<std::convertible_to<int>... Signals>
    signal_set(
        capy::execution_context& ctx,
        int signal,
        Signals... signals)
        : signal_set(ctx)
    {
        add(signal).value();
        (add(signals).value(), ...);
    }

    /** Move constructor.

        Transfers ownership of the signal set resources.

        @param other The signal set to move from.
    */
    signal_set(signal_set&& other) noexcept;

    /** Move assignment operator.

        Closes any existing signal set and transfers ownership.
        The source and destination must share the same execution context.

        @param other The signal set to move from.

        @return Reference to this signal set.

        @throws std::logic_error if the signal sets have different
            execution contexts.
    */
    signal_set& operator=(signal_set&& other);

    signal_set(signal_set const&) = delete;
    signal_set& operator=(signal_set const&) = delete;

    /** Add a signal to the signal set.

        This function adds the specified signal to the set. It has no
        effect if the signal is already in the set.

        @param signal_number The signal to be added to the set.

        @return Success, or an error if the signal could not be added.
    */
    system::result<void> add(int signal_number);

    /** Remove a signal from the signal set.

        This function removes the specified signal from the set. It has
        no effect if the signal is not in the set.

        @param signal_number The signal to be removed from the set.

        @return Success, or an error if the signal could not be removed.
    */
    system::result<void> remove(int signal_number);

    /** Remove all signals from the signal set.

        This function removes all signals from the set. It has no effect
        if the set is already empty.

        @return Success, or an error if resetting any signal handler fails.
    */
    system::result<void> clear();

    /** Cancel all operations associated with the signal set.

        This function forces the completion of any pending asynchronous
        wait operations against the signal set. The handler for each
        cancelled operation will be invoked with capy::error::canceled.

        Cancellation does not alter the set of registered signals.
    */
    void cancel();

    /** Wait for a signal to be delivered.

        The operation supports cancellation via `std::stop_token` through
        the affine awaitable protocol. If the associated stop token is
        triggered, the operation completes immediately with
        `capy::error::canceled`.

        @return An awaitable that completes with `io_result<int>`.
            Returns the signal number when a signal is delivered,
            or an error code on failure including:
            - capy::error::canceled: Cancelled via stop_token or cancel().
    */
    auto async_wait()
    {
        return wait_awaitable(*this);
    }

private:
    signal_set_impl& get() const noexcept
    {
        return *static_cast<signal_set_impl*>(impl_);
    }
};

} // namespace corosio
} // namespace boost

#endif
