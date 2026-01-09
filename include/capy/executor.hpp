//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CAPY_EXECUTOR_HPP
#define CAPY_EXECUTOR_HPP

#include <capy/config.hpp>
#include <capy/executor_work.hpp>

#include <concepts>

namespace capy {

/** Abstract base class for executors.

    Executors provide the interface for dispatching coroutines and posting
    work items. This abstract base enables type-erased storage of executors
    in coroutine promises via `executor_base const&`.

    @see io_context::executor
*/
struct executor_base
{
    virtual ~executor_base() = default;
    virtual coro dispatch(coro h) const = 0;
    virtual void post(executor_work* w) const = 0;

    // meet the requirements of dispatcher (affine awaitables)
    coro operator()(coro h) const { return this->dispatch(h); }
};


/** A concept for types that are executors.

    An executor is responsible for scheduling and running asynchronous
    operations. It provides mechanisms for symmetric transfer of coroutine
    handles and for queuing work items to be executed later.

    This concept requires that `T` is unambiguously derived from `executor_base`
    and is both copy and move constructible (executors are passed by value).
    The `executor_base` base class defines the interface that executors must
    implement: `dispatch()` for coroutine handles and `post()` for work items.

    @tparam T The type to check for executor conformance.
*/
template<class T>
concept executor =
    std::derived_from<T, executor_base> &&
    std::copy_constructible<T> &&
    std::move_constructible<T>;

} // namespace capy

#endif
