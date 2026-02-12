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

#include <boost/corosio/basic_io_context.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/detail/type_id.hpp>
#include <coroutine>

namespace boost::corosio::detail {

/** Returns a handle for symmetric transfer on I/O completion.

    If the executor is io_context::executor_type, returns `h`
    directly (fast path). Otherwise dispatches through the
    executor, which returns `h` or `noop_coroutine()`.

    Callers in coroutine machinery should return the result
    for symmetric transfer. Callers at the scheduler pump
    level should call `.resume()` on the result.

    @param ex The executor to dispatch through.
    @param h The coroutine handle to resume.

    @return A handle for symmetric transfer or `std::noop_coroutine()`.
*/
inline std::coroutine_handle<>
dispatch_coro(
    capy::executor_ref ex,
    std::coroutine_handle<> h)
{
    if ( ex.target< basic_io_context::executor_type >() )
        return h;
    return ex.dispatch(h);
}

} // namespace boost::corosio::detail

#endif
