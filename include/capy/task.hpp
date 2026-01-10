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
#include <capy/make_affine.hpp>

#include <exception>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace capy {

namespace detail {

// Helper base for result storage and return_void/return_value
template<typename T>
struct task_return_base
{
    std::optional<T> result_;

    void return_value(T value)
    {
        result_ = std::move(value);
    }
};

template<>
struct task_return_base<void>
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

    The task uses `[[clang::coro_await_elidable]]` (when available) to enable
    heap allocation elision optimization (HALO) for nested coroutine calls.

    @see any_dispatcher
*/
template<typename T = void>
struct [[nodiscard]] CAPY_CORO_AWAIT_ELIDABLE
    task
{
    struct promise_type
        : frame_allocating_base
        , detail::task_return_base<T>
    {
        any_dispatcher ex_;
        any_dispatcher caller_ex_;
        coro continuation_;
        std::exception_ptr ep_;
        std::stop_token stop_token_;
        bool needs_dispatch_ = false;

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

                coro await_suspend(coro) const noexcept
                {
                    if(p_->continuation_)
                    {
                        // Same dispatcher: true symmetric transfer
                        if(!p_->needs_dispatch_)
                            return p_->continuation_;
                        return p_->caller_ex_(p_->continuation_);
                    }
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
                return a_.await_resume();
            }

            template<class Promise>
            auto await_suspend(std::coroutine_handle<Promise> h)
            {
                using A = std::decay_t<Awaitable>;
                if constexpr (stoppable_awaitable<A, any_dispatcher>)
                    return a_.await_suspend(h, p_->ex_, p_->stop_token_);
                else
                    return a_.await_suspend(h, p_->ex_);
            }
        };

        template<class Awaitable>
        auto await_transform(Awaitable&& a)
        {
            using A = std::decay_t<Awaitable>;
            if constexpr (affine_awaitable<A, any_dispatcher>)
            {
                // Zero-overhead path for affine awaitables
                return transform_awaiter<Awaitable>{
                    std::forward<Awaitable>(a), this};
            }
            else
            {
                // Trampoline fallback for legacy awaitables
                return make_affine(std::forward<Awaitable>(a), ex_);
            }
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
        h_.promise().needs_dispatch_ = false;
        return h_;
    }

    // Stoppable awaitable: receive caller's dispatcher and stop_token
    template<dispatcher D>
    coro await_suspend(coro continuation, D const& caller_ex, std::stop_token token)
    {
        h_.promise().caller_ex_ = caller_ex;
        h_.promise().continuation_ = continuation;
        h_.promise().ex_ = caller_ex;
        h_.promise().stop_token_ = token;
        h_.promise().needs_dispatch_ = false;
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
static_assert(stoppable_awaitable<task<void>, any_dispatcher>);
static_assert(stoppable_awaitable<task<int>, any_dispatcher>);

} // namespace capy

#endif
