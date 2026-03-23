//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_DISPATCH_CORO_HPP
#define BOOST_COROSIO_DETAIL_DISPATCH_CORO_HPP

#include <boost/corosio/io_context.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/detail/type_id.hpp>
#include <coroutine>

namespace boost::corosio::detail {

/** Returns a handle for symmetric transfer on I/O completion.

    If the executor is io_context::executor_type, returns `c.h`
    directly (fast path). Otherwise dispatches through the
    executor, which returns `c.h` or `noop_coroutine()`.

    Callers in coroutine machinery should return the result
    for symmetric transfer. Callers at the scheduler pump
    level should call `.resume()` on the result.

    @param ex The executor to dispatch through.
    @param c The continuation to dispatch. Must remain at a
             stable address until dequeued by the executor.

    @return A handle for symmetric transfer or `std::noop_coroutine()`.
*/
inline std::coroutine_handle<>
dispatch_coro(capy::executor_ref ex, capy::continuation& c)
{
    if (ex.target<io_context::executor_type>() != nullptr)
        return c.h;
    return ex.dispatch(c);
}

} // namespace boost::corosio::detail

#endif
