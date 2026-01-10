//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CAPY_RUN_ON_HPP
#define CAPY_RUN_ON_HPP

#include <capy/config.hpp>
#include <capy/affine.hpp>
#include <capy/executor.hpp>
#include <capy/task.hpp>

#include <utility>

namespace capy {

/** Awaitable that binds a task to a specific executor.

    Stores the executor by value. When co_awaited, the co_await
    expression's lifetime extension keeps the executor alive for
    the duration of the operation.

    @tparam T The task's return type
    @tparam E The executor type
*/
template<typename T, executor E>
struct run_on_awaitable
{
    E ex_;
    std::coroutine_handle<typename task<T>::promise_type> h_;

    run_on_awaitable(
        E ex,
        std::coroutine_handle<typename task<T>::promise_type> h)
        : ex_(std::move(ex))
        , h_(h)
    {
    }

    bool await_ready() const noexcept
    {
        return false;
    }

    auto await_resume()
    {
        if constexpr (std::is_void_v<T>)
            return;
        else
            return std::move(*h_.promise().result_);
    }

    // Affine awaitable: receives caller's dispatcher for completion dispatch
    template<dispatcher D>
    coro await_suspend(coro continuation, D const& caller_ex)
    {
        // 'this' is kept alive by co_await until completion
        // ex_ is valid for the entire operation
        h_.promise().ex_ = ex_;
        h_.promise().caller_ex_ = caller_ex;
        h_.promise().continuation_ = continuation;
        h_.promise().needs_dispatch_ = true;
        return h_;
    }

    // Detached execution (no coroutine caller)
    // Precondition: 'this' is heap-allocated
    coro await_suspend_detached()
    {
        h_.promise().ex_ = ex_;
        h_.promise().caller_ex_ = ex_;
        h_.promise().continuation_ = nullptr;
        h_.promise().needs_dispatch_ = false;
        h_.promise().detached_cleanup_ = +[](void* p) {
            delete static_cast<run_on_awaitable*>(p);
        };
        h_.promise().detached_state_ = this;
        return h_;
    }

    ~run_on_awaitable()
    {
        if(h_ && !h_.done())
            h_.destroy();
    }

    // Non-copyable
    run_on_awaitable(run_on_awaitable const&) = delete;
    run_on_awaitable& operator=(run_on_awaitable const&) = delete;

    // Movable
    run_on_awaitable(run_on_awaitable&& other) noexcept
        : ex_(std::move(other.ex_))
        , h_(std::exchange(other.h_, nullptr))
    {
    }

    run_on_awaitable& operator=(run_on_awaitable&& other) noexcept
    {
        if(this != &other)
        {
            if(h_ && !h_.done())
                h_.destroy();
            ex_ = std::move(other.ex_);
            h_ = std::exchange(other.h_, nullptr);
        }
        return *this;
    }
};

/** Binds a task to execute on a specific executor.

    The executor is stored by value in the returned awaitable.
    When co_awaited, the inner task receives this executor through
    direct promise configuration.

    @param ex The executor on which the task should run (copied by value).
    @param t The task to bind to the executor.

    @return An awaitable that runs t on the specified executor.
*/
template<executor Executor, typename T>
auto run_on(Executor ex, task<T> t)
{
    return run_on_awaitable<T, Executor>{
        std::move(ex), t.release()};
}

} // namespace capy

#endif
