//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CAPY_TASK_HPP
#define CAPY_TASK_HPP

#include <capy/config.hpp>
#include <capy/affine.hpp>
#include <capy/detail/frame_pool.hpp>
#include <capy/executor.hpp>
#include <capy/frame_allocator.hpp>

#include <exception>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace capy {

namespace detail {

// Helper base to provide return_void or return_value without full class specialization
template<typename T, typename Derived>
struct task_return_base
{
    void return_value(T value)
    {
        static_cast<Derived*>(this)->result_ = std::move(value);
    }
};

template<typename Derived>
struct task_return_base<void, Derived>
{
    void return_void()
    {
    }
};

} // namespace detail

/** A coroutine task type implementing the affine awaitable protocol.

    This task type represents an asynchronous operation that can be awaited.
    It implements the affine awaitable protocol where `await_suspend` receives
    the caller's executor, enabling proper completion dispatch across executor
    boundaries.

    @tparam T The return type of the task. Defaults to void.

    Key features:
    @li Lazy execution - the coroutine does not start until awaited
    @li Symmetric transfer - uses coroutine handle returns for efficient
   resumption
    @li Executor inheritance - inherits caller's executor unless explicitly
   bound
    @li Custom frame allocation - supports frame allocators via first/second
   parameter

    The task uses `[[clang::coro_await_elidable]]` (when available) to enable
    heap allocation elision optimization (HALO) for nested coroutine calls.

    @par Frame Allocation
    The promise type provides custom operator new overloads that detect
    `has_frame_allocator` on the first or second coroutine parameter,
    enabling pooled allocation of coroutine frames.

    @see executor_base
    @see has_frame_allocator
    @see corosio::detail::frame_pool
*/
template<typename T = void>
struct CAPY_CORO_AWAIT_ELIDABLE
    task
{
    struct promise_type
        : capy::detail::frame_pool::promise_allocator
        , detail::task_return_base<T, promise_type>
    {
        executor_base const* ex_ = nullptr;
        executor_base const* caller_ex_ = nullptr;
        coro continuation_;

        // Detached cleanup support for async_run
        void (*detached_cleanup_)(void*) = nullptr;
        void* detached_state_ = nullptr;

        // Storage: monostate for void, optional<T> otherwise
        [[no_unique_address]]
        std::conditional_t<
            std::is_void_v<T>,
            std::monostate,
            std::optional<T>> result_;

        task get_return_object()
        {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept
        {
            return {};
        }

        auto final_suspend() noexcept
        {
            struct awaiter
            {
                promise_type* p_;

                bool await_ready() const noexcept
                {
                    return false;
                }

                coro await_suspend(coro h) const noexcept
                {
                    // Destroy before dispatch enables memory recycling
                    auto continuation = p_->continuation_;
                    auto caller_ex = p_->caller_ex_;
                    auto detached_cleanup = p_->detached_cleanup_;
                    auto detached_state = p_->detached_state_;
                    h.destroy();
                    if(continuation)
                        return caller_ex->dispatch(continuation);
                    if(detached_cleanup)
                        detached_cleanup(detached_state);
                    return std::noop_coroutine();
                }

                void await_resume() const noexcept
                {
                }
            };
            return awaiter{this};
        }

        // return_void() or return_value() inherited from task_return_base

        void unhandled_exception()
        {
            std::terminate();
        }

        template<class Awaitable>
        struct transform_awaiter
        {
            std::decay_t<Awaitable> a_;
            promise_type* p_;

            bool await_ready()
            {
                return a_.await_ready();
            }

            auto await_resume()
            {
                return a_.await_resume();
            }

            template<class Promise>
            auto await_suspend(std::coroutine_handle<Promise> h)
            {
                return a_.await_suspend(h, *p_->ex_);
            }
        };

        template<class Awaitable>
        auto await_transform(Awaitable&& a)
        {
            return transform_awaiter<Awaitable>{std::forward<Awaitable>(a), this};
        }
    };

    std::coroutine_handle<promise_type> h_;

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

    // Affine awaitable: receive caller's executor for completion dispatch
    coro await_suspend(coro continuation, executor_base const& caller_ex)
    {
        static_assert(dispatcher<executor_base>);
        h_.promise().caller_ex_ = &caller_ex;
        h_.promise().continuation_ = continuation;
        h_.promise().ex_ = &caller_ex;
        return h_;
    }

    void start(executor_base const& ex)
    {
        h_.promise().ex_ = &ex;
        h_.promise().caller_ex_ = &ex;
        h_.resume();
    }

    /** Release ownership of the coroutine handle.

        After calling this, the task no longer owns the handle and will
        not destroy it. The caller is responsible for the handle's lifetime.

        @return The coroutine handle, or nullptr if already released.
    */
    std::coroutine_handle<promise_type> release() noexcept
    {
        return std::exchange(h_, nullptr);
    }

    ~task()
    {
        if(h_ && !h_.done())
            h_.destroy();
    }

    // Non-copyable
    task(task const&) = delete;
    task& operator=(task const&) = delete;

    // Movable
    task(task&& other) noexcept
        : h_(std::exchange(other.h_, nullptr))
    {
    }

    task& operator=(task&& other) noexcept
    {
        if(this != &other)
        {
            if(h_ && !h_.done())
                h_.destroy();
            h_ = std::exchange(other.h_, nullptr);
        }
        return *this;
    }

private:
    explicit task(std::coroutine_handle<promise_type> h)
        : h_(h)
    {
    }
};

static_assert(affine_awaitable<task<void>, executor_base>);
static_assert(affine_awaitable<task<int>, executor_base>);

} // namespace capy

#endif
