//0
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
#include <capy/frame_allocator.hpp>

#include <exception>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace capy {

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

    The task uses `[[clang::coro_await_elidable]]` (when available) to enable
    heap allocation elision optimization (HALO) for nested coroutine calls.

    @see any_dispatcher
*/
template<typename T = void>
struct CAPY_CORO_AWAIT_ELIDABLE
    task
{
    // Helper base for result storage and return_void/return_value
    template<typename U>
    struct return_base
    {
        std::optional<U> result_;

        void return_value(U value)
        {
            result_ = std::move(value);
        }
    };

    template<>
    struct return_base<void>
    {
        void return_void()
        {
        }
    };

    struct promise_type
        : frame_allocating_base
        , return_base<T>
    {
        any_dispatcher ex_;
        any_dispatcher caller_ex_;
        coro continuation_;
        std::exception_ptr ep_;

        // Detached cleanup support for async_run
        void (*detached_cleanup_)(void*) = nullptr;
        void* detached_state_ = nullptr;

        task get_return_object()
        {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        auto initial_suspend() noexcept
        {
            struct awaiter
            {
                promise_type* p_;

                bool await_ready() const noexcept { return false; }
                void await_suspend(coro) const noexcept {}
                void await_resume() const noexcept
                {
                    p_->restore_frame_allocator();
                }
            };
            return awaiter{this};
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

                coro await_suspend(coro) const noexcept
                {
                    if(p_->continuation_)
                    {
                        // Same dispatcher: true symmetric transfer
                        if(p_->caller_ex_ == p_->ex_)
                            return p_->continuation_;
                        return p_->caller_ex_(p_->continuation_);
                    }
                    if(p_->detached_cleanup_)
                        p_->detached_cleanup_(p_->detached_state_);
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
            ep_ = std::current_exception();
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
                p_->restore_frame_allocator();
                return a_.await_resume();
            }

            template<class Promise>
            auto await_suspend(std::coroutine_handle<Promise> h)
            {
                return a_.await_suspend(h, p_->ex_);
            }
        };

        template<class Awaitable>
        auto await_transform(Awaitable&& a)
        {
            return transform_awaiter<Awaitable>{std::forward<Awaitable>(a), this};
        }
    };

    std::coroutine_handle<promise_type> h_;

    ~task()
    {
        if(h_)
            h_.destroy();
    }

    bool await_ready() const noexcept
    {
        return false;
    }

    auto await_resume()
    {
        if(h_.promise().ep_)
            std::rethrow_exception(h_.promise().ep_);
        if constexpr (! std::is_void_v<T>)
            return std::move(*h_.promise().result_);
        else
            return;
    }

    // Affine awaitable: receive caller's dispatcher for completion dispatch
    template<dispatcher D>
    coro await_suspend(coro continuation, D const& caller_ex)
    {
        h_.promise().caller_ex_ = caller_ex;
        h_.promise().continuation_ = continuation;
        h_.promise().ex_ = caller_ex;
        h_.promise().alloc_ = frame_allocating_base::get_frame_allocator();
        return h_;
    }

    /** Release ownership of the coroutine handle.

        After calling this, the task no longer owns the handle and will
        not destroy it. The caller is responsible for the handle's lifetime.

        @return The coroutine handle, or nullptr if already released.
    */
    auto release() noexcept ->
        std::coroutine_handle<promise_type>
    {
        return std::exchange(h_, nullptr);
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
            if(h_)
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

static_assert(affine_awaitable<task<void>, any_dispatcher>);
static_assert(affine_awaitable<task<int>, any_dispatcher>);

} // namespace capy

#endif
