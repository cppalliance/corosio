//
// affine.hpp
//
// Concepts for affine awaitables.
//
// This header provides the core concepts for zero-overhead scheduler affinity:
//
// - dispatcher: concept for types callable with coroutine handles
// - affine_awaitable: concept for awaitables that accept a dispatcher
//
// For helper types and functions, see affine_helpers.hpp
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

    @see affine-awaitable-protocol.md §1
    @see affine-awaitables.md §4.1
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

    @see affine-awaitable-protocol.md §2
    @see affine-awaitables.md §4.1, §4.3
*/
template<typename A, typename D, typename P = void>
concept affine_awaitable = dispatcher<D, P> &&
    requires(A a, std::coroutine_handle<P> h, D const& d) { a.await_suspend(h, d); };

#endif // CAPY_AFFINE_HPP
