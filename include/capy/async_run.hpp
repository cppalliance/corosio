//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CAPY_ASYNC_RUN_HPP
#define CAPY_ASYNC_RUN_HPP

#include <capy/config.hpp>
#include <capy/detail/root_task.hpp>
#include <capy/task.hpp>

#include <utility>

namespace capy {

template<class Executor>
    requires dispatcher<Executor>
detail::root_task<Executor> detail::wrapper(task<> t)
{
    co_await std::move(t);
}

/** Starts a task for execution on an executor.

    This function initiates execution of a task by posting it to the
    specified executor's work queue. The task will begin running when
    the executor processes the posted work item.

    The task is "fire and forget" - it will self-destruct upon completion.
    There is no mechanism to wait for the result or retrieve exceptions;
    unhandled exceptions will call `std::terminate()`.

    @param ex The executor on which to run the task.
    @param t The task to execute.

    @par Example
    @code
    io_context ioc;
    async_run(ioc.get_executor(), my_coroutine());
    ioc.run();
    @endcode

    @note The executor is captured by value to ensure it remains valid
    for the duration of the task's execution.
*/
template<class Executor>
void async_run(Executor ex, task<> t)
{
    auto root = detail::wrapper<Executor>(std::move(t));
    root.h_.promise().ex_ = std::move(ex);
    root.h_.promise().starter_.h_ = root.h_;
    root.h_.promise().ex_.post(&root.h_.promise().starter_);
    root.release();
}

} // namespace capy

#endif
