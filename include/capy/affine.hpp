//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CAPY_AFFINE_HPP
#define CAPY_AFFINE_HPP

#include <capy/config.hpp>
#include <concepts>
#include <coroutine>

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
    - `D(h)` must return a `std::coroutine_handle<>` (or convertible type)
      to enable symmetric transfer
    - Calling `D(h)` schedules `h` for resumption (typically by scheduling
      it on a specific execution context) and returns a coroutine handle
      that the caller may use for symmetric transfer
    - The dispatcher must be const-callable (logical constness), enabling
      thread-safe concurrent dispatch from multiple coroutines

    @note Since `std::coroutine_handle<>` has `operator()` which invokes
    `resume()`, the handle itself is callable and can be dispatched directly.
*/
template<typename D, typename P = void>
concept dispatcher = requires(D const& d, std::coroutine_handle<P> h) {
    { d(h) } -> std::convertible_to<std::coroutine_handle<>>;
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
        auto await_suspend(std::coroutine_handle<> h, Dispatcher const& d) {
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
    std::coroutine_handle<>(*f_)(
        void const*,
        std::coroutine_handle<>);

public:
    /** Default constructor.

        Constructs an empty `any_dispatcher`. Calling `operator()` on a
        default-constructed instance results in undefined behavior.
    */
    any_dispatcher() = default;

    /** Constructs from any dispatcher type.

        Captures a reference to the given dispatcher and stores a type-erased
        invocation function. The dispatcher must remain valid for the lifetime
        of this `any_dispatcher` instance.

        @param d The dispatcher to wrap. Must satisfy the `dispatcher` concept.
                 A pointer to this object is stored internally; the dispatcher
                 must outlive this wrapper.
    */
    template<dispatcher D>
    any_dispatcher(
        D const& d)
        : d_(&d)
        , f_([](void const* pd, std::coroutine_handle<> h)
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
    auto
    operator()(
        std::coroutine_handle<> h) const ->
            std::coroutine_handle<>
    {
        return f_(d_, h);
    }
};

} // namespace capy

#endif

