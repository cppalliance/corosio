//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_RESUME_CORO_HPP
#define BOOST_COROSIO_DETAIL_RESUME_CORO_HPP

#include <boost/corosio/basic_io_context.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/detail/type_id.hpp>
#include <boost/capy/coro.hpp>

namespace boost::corosio::detail {

/** Resumes a coroutine for I/O completion.

    If the executor is io_context::executor_type, resumes directly
    to avoid dispatch overhead. Otherwise dispatches through the
    executor. No memory fence is needed since GQCS/epoll_wait
    provide acquire semantics.

    @param d The executor to dispatch through.
    @param h The coroutine handle to resume.
*/
inline void
resume_coro(capy::executor_ref d, capy::coro h)
{
    // Fast path: resume directly for io_context executor
    if (&d.type_id() == &capy::detail::type_id<basic_io_context::executor_type>())
        h.resume();
    else
        d.dispatch(h);
}

} // namespace boost::corosio::detail

#endif
