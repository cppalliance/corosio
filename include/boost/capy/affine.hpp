//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_CAPY_AFFINE_HPP
#define BOOST_CAPY_AFFINE_HPP

#include <boost/capy/config.hpp>

#include <concepts>
#include <stop_token>

namespace boost {
namespace capy {

/** Concept for dispatcher types.

    A dispatcher is a callable object that accepts a coroutine handle
    and schedules it for resumption. The dispatcher is responsible for
    ensuring the handle is eventually resumed on the appropriate execution
    context.

    @tparam D The dispatcher type
    @tparam P The promise type (defaults to void)

    @par Requirements
    - `D(h)` must be valid where `h` is `std::coroutine_handle<P>` and
      `d` is a const reference to `D`
    - `D(h)` must return a `coro` (or convertible type)
      to enable symmetric transfer
    - Calling `D(h)` schedules `h` for resumption (typically by scheduling
      it on a specific execution context) and returns a coroutine handle
      that the caller may use for symmetric transfer
    - The dispatcher must be const-callable (logical constness), enabling
      thread-safe concurrent dispatch from multiple coroutines

    @note Since `coro` has `operator()` which invokes
    `resume()`, the handle itself is callable and can be dispatched directly.
*/
template<typename D, typename P = void>
concept dispatcher = requires(D const& d, std::coroutine_handle<P> h) {
    { d(h) } -> std::convertible_to<coro>;
};

/** Concept for affine awaitable types.

    An awaitable is affine if it participates in the affine awaitable protocol
    by accepting a dispatcher in its `await_suspend` method. This enables
    zero-overhead scheduler affinity without requiring the full sender/receiver
    protocol.

    @tparam A The awaitable type
    @tparam D The dispatcher type
    @tparam P The promise type (defaults to void)

    @par Requirements
    - `D` must satisfy `dispatcher<D, P>`
    - `A` must provide `await_suspend(std::coroutine_handle<P> h, D const& d)`
    - The awaitable must use the dispatcher `d` to resume the caller, e.g. `return d(h);`
    - The dispatcher returns a coroutine handle that `await_suspend` may return for symmetric
   transfer

    @par Example
    @code
    struct my_async_op {
        template<typename Dispatcher>
        auto await_suspend(coro h, Dispatcher const& d) {
            start_async([h, &d] {
                d(h);  // Schedule resumption through dispatcher
            });
            return std::noop_coroutine();  // Or return d(h) for symmetric transfer
        }
        // ... await_ready, await_resume ...
    };
    @endcode
*/
template<typename A, typename D, typename P = void>
concept affine_awaitable = dispatcher<D, P> &&
    requires(A a, std::coroutine_handle<P> h, D const& d) { a.await_suspend(h, d); };

/** Concept for stoppable awaitable types.

    An awaitable is stoppable if it participates in the stoppable awaitable
    protocol by accepting both a dispatcher and a stop_token in its
    `await_suspend` method. This extends the affine awaitable protocol to
    enable automatic stop token propagation through coroutine chains.

    @tparam A The awaitable type
    @tparam D The dispatcher type
    @tparam P The promise type (defaults to void)

    @par Requirements
    - `A` must satisfy `affine_awaitable<A, D, P>`
    - `A` must provide `await_suspend(std::coroutine_handle<P> h, D const& d,
      std::stop_token token)`
    - The awaitable should use the stop_token to support cancellation
    - The awaitable must use the dispatcher `d` to resume the caller

    @par Example
    @code
    struct my_stoppable_op {
        template<typename Dispatcher>
        auto await_suspend(coro h, Dispatcher const& d, std::stop_token token) {
            start_async([h, &d, token] {
                if (token.stop_requested()) {
                    // Handle cancellation
                }
                d(h);  // Schedule resumption through dispatcher
            });
            return std::noop_coroutine();
        }
        // ... await_ready, await_resume ...
    };
    @endcode

    @see affine_awaitable
    @see dispatcher
*/
template<typename A, typename D, typename P = void>
concept stoppable_awaitable = affine_awaitable<A, D, P> &&
    requires(A a, std::coroutine_handle<P> h, D const& d, std::stop_token token) {
        a.await_suspend(h, d, token);
    };

/** A type-erased wrapper for dispatcher objects.

    This class provides type erasure for any type satisfying the `dispatcher`
    concept, enabling runtime polymorphism without virtual functions. It stores
    a pointer to the original dispatcher and a function pointer to invoke it,
    allowing dispatchers of different types to be stored uniformly.

    @par Thread Safety
    The `any_dispatcher` itself is not thread-safe for concurrent modification,
    but `operator()` is const and safe to call concurrently if the underlying
    dispatcher supports concurrent dispatch.

    @par Lifetime
    The `any_dispatcher` stores a pointer to the original dispatcher object.
    The caller must ensure the referenced dispatcher outlives the `any_dispatcher`
    instance. This is typically satisfied when the dispatcher is an executor
    stored in a coroutine promise or service provider.

    @par Example
    @code
    void store_dispatcher(any_dispatcher d) {
        // Can store any dispatcher type uniformly
        auto h = d(some_coroutine);  // Invoke through type-erased interface
    }

    executor_base const& ex = get_executor();
    store_dispatcher(ex);  // Implicitly converts to any_dispatcher
    @endcode

    @see dispatcher
    @see executor_base
*/
class any_dispatcher
{
    void const* d_ = nullptr;
    coro(*f_)(void const*, coro);

public:
    /** Default constructor.

        Constructs an empty `any_dispatcher`. Calling `operator()` on a
        default-constructed instance results in undefined behavior.
    */
    any_dispatcher() = default;

    /** Copy constructor.

        Copies the internal pointer and function, preserving identity.
        This enables the same-dispatcher optimization when passing
        any_dispatcher through coroutine chains.
    */
    any_dispatcher(any_dispatcher const&) = default;
    any_dispatcher& operator=(any_dispatcher const&) = default;

    /** Constructs from any dispatcher type.

        Captures a reference to the given dispatcher and stores a type-erased
        invocation function. The dispatcher must remain valid for the lifetime
        of this `any_dispatcher` instance.

        @param d The dispatcher to wrap. Must satisfy the `dispatcher` concept.
                 A pointer to this object is stored internally; the dispatcher
                 must outlive this wrapper.
    */
    template<dispatcher D>
        requires (!std::same_as<std::decay_t<D>, any_dispatcher>)
    any_dispatcher(
        D const& d)
        : d_(&d)
        , f_([](void const* pd, coro h)
            {
                D const& d = *static_cast<D const*>(pd);
                return d(h);
            })
    {
    }

    /** Returns true if this instance holds a valid dispatcher.

        @return `true` if constructed with a dispatcher, `false` if
                default-constructed.
    */
    explicit
    operator bool() const noexcept
    {
        return d_ != nullptr;
    }

    /** Compares two dispatchers for identity.

        Two `any_dispatcher` instances are equal if they wrap the same
        underlying dispatcher object (pointer equality). This enables
        the affinity optimization: when `caller_dispatcher == my_dispatcher`,
        symmetric transfer can proceed without a `running_in_this_thread()`
        check.

        @param other The dispatcher to compare against.

        @return `true` if both wrap the same dispatcher object.
    */
    bool
    operator==(
        any_dispatcher const& other) const noexcept
    {
        return d_ == other.d_;
    }

    /** Dispatches a coroutine handle through the wrapped dispatcher.

        Invokes the stored dispatcher with the given coroutine handle,
        returning a handle suitable for symmetric transfer.

        @param h The coroutine handle to dispatch for resumption.

        @return A coroutine handle that the caller may use for symmetric
                transfer, or `std::noop_coroutine()` if the dispatcher
                posted the work for later execution.

        @pre This instance was constructed with a valid dispatcher
             (not default-constructed).
    */
    coro
    operator()(coro h) const
    {
        return f_(d_, h);
    }
};

} // namespace capy
} // namespace boost

#endif
