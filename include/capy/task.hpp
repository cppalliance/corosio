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

#include <coroutine>
#include <exception>

namespace capy {

/** A coroutine task type implementing the affine awaitable protocol.

    This task type represents an asynchronous operation that can be awaited.
    It implements the affine awaitable protocol where `await_suspend` receives
    the caller's executor, enabling proper completion dispatch across executor
    boundaries.

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
struct CAPY_CORO_AWAIT_ELIDABLE task
{
    struct promise_type : capy::detail::frame_pool::promise_allocator
    {
        executor_base const* ex_ = nullptr;
        executor_base const* caller_ex_ = nullptr;
        coro continuation_;

        task get_return_object()
        {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }

        auto final_suspend() noexcept
        {
            struct awaiter
            {
                promise_type* p_;
                bool await_ready() const noexcept { return false; }
                std::coroutine_handle<> await_suspend(coro h) const noexcept
                {
                    std::coroutine_handle<> next = std::noop_coroutine();
                    if(p_->continuation_)
                        next = p_->caller_ex_->dispatch(p_->continuation_);
                    h.destroy();
                    // Return continuation handle for symmetric transfer to
                    // avoid stack growth when resuming the caller
                    return next;
                }
                void await_resume() const noexcept {}
            };
            return awaiter{this};
        }

        void return_void() {}
        void unhandled_exception() { std::terminate(); }

        template<class Awaitable>
        struct transform_awaiter
        {
            std::decay_t<Awaitable> a_;
            promise_type* p_;
            bool await_ready() { return a_.await_ready(); }
            auto await_resume() { return a_.await_resume(); }
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

        void set_executor(executor_base const& ex) { ex_ = &ex; }
    };

    std::coroutine_handle<promise_type> h_;
    bool has_own_ex_ = false;

    bool await_ready() const noexcept { return false; }
    void await_resume() const noexcept {}
    // Affine awaitable: receive caller's executor for completion dispatch
    std::coroutine_handle<> await_suspend(coro continuation, executor_base const& caller_ex)
    {
        static_assert(dispatcher<executor_base>);
        h_.promise().caller_ex_ = &caller_ex;
        h_.promise().continuation_ = continuation;

        if(has_own_ex_)
        {
            struct starter : executor_work
            {
                coro h_;
                starter(coro h) : h_(h) {}
                void operator()() override
                {
                    h_.resume();
                    destroy();
                }
                void destroy() override { delete this; }
                virtual ~starter() = default;
            };
            // VFALCO this should be dispatch() when it handles
            // running_in_this_thread()
            h_.promise().ex_->post(new starter{h_});
            // Return noop because we posted work; executor will resume us later
            return std::noop_coroutine();
        }
        else
        {
            // Return our handle for symmetric transfer to avoid stack growth
            h_.promise().ex_ = &caller_ex;
            return h_;
        }
    }

    void start(executor_base const& ex)
    {
        h_.promise().set_executor(ex);
        h_.promise().caller_ex_ = &ex;
        h_.resume();
    }

    void set_executor(executor_base const& ex)
    {
        h_.promise().ex_ = &ex;
        has_own_ex_ = true;
    }
};

static_assert(affine_awaitable<task, executor_base>);

} // namespace capy

#endif

