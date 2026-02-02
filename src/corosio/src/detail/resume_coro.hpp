//
// Copyright (c) 2026 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_RESUME_CORO_HPP
#define BOOST_COROSIO_DETAIL_RESUME_CORO_HPP

#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/coro.hpp>
#include <atomic>

namespace boost::corosio::detail {

/** Resumes a coroutine with proper memory synchronization.

    The acquire fence ensures all I/O results (buffer contents,
    error codes, bytes transferred) written by other threads are
    visible to the resumed coroutine before it continues execution.

    Uses symmetric transfer: if dispatch returns the same handle,
    we resume directly. If it returns noop_coroutine, the work was
    posted to a queue and will be resumed by the scheduler.

    @param d The executor to dispatch through.
    @param h The coroutine handle to resume.
*/
inline void
resume_coro(capy::executor_ref d, capy::coro h)
{
    // I/O results may have been written by another thread (OS or worker).
    // Acquire fence ensures those writes are visible before coroutine resumes.
    std::atomic_thread_fence(std::memory_order_acquire);
    auto resume_h = d.dispatch(h);
    if (resume_h.address() == h.address())
        resume_h.resume();
}

} // namespace boost::corosio::detail

#endif
