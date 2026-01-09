//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CAPY_DETAIL_ROOT_TASK_HPP
#define CAPY_DETAIL_ROOT_TASK_HPP

#include <capy/affine.hpp>
#include <capy/task.hpp>
#include <capy/detail/frame_pool.hpp>

#include <coroutine>
#include <exception>

namespace capy::detail {

template<class Executor>
    requires dispatcher<Executor>
struct root_task
{
    struct starter : executor_work
    {
        coro h_;

        void operator()() override
        {
            h_.resume();
            // no need to delete this; owned by promise_type
        }

        void destroy() override
        {
            // Not meant to be destroyed; owned by promise_type
        }
    };

    struct promise_type : capy::detail::frame_pool::promise_allocator
    {
        Executor ex_;
        starter starter_;

        root_task get_return_object()
        {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }

        auto final_suspend() noexcept
        {
            struct awaiter
            {
                bool await_ready() const noexcept { return false; }
                std::coroutine_handle<> await_suspend(coro h) const noexcept
                {
                    h.destroy();
                    return std::noop_coroutine();
                }
                void await_resume() const noexcept {}
            };
            return awaiter{};
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
                static_assert(dispatcher<Executor>);
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

    void release() { h_ = nullptr; }

    ~root_task()
    {
        if(h_)
            h_.destroy();
    }
};

template<class Executor>
    requires dispatcher<Executor>
root_task<Executor> wrapper(task<> t);

} // namespace capy::detail

#endif
