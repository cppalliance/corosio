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
#include <capy/task.hpp>

namespace capy {

/** Binds a task to execute on a specific executor.

    This function sets the executor for a task, causing it to run on the
    specified executor rather than inheriting the caller's executor. When
    awaited, the task will be posted to its bound executor instead of
    executing inline via symmetric transfer.

    @param ex The executor on which the task should run.
    @param t The task to bind to the executor.

    @return The same task, now bound to the specified executor.

    @par Example
    @code
    co_await run_on(strand, some_task());
    @endcode
*/
task run_on(executor_base const& ex, task t)
{
    t.set_executor(ex);
    return t;
}

} // namespace capy

#endif
