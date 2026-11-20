//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_COMPLETION_KEY_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_COMPLETION_KEY_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/native/detail/iocp/win_windows.hpp>

namespace boost::corosio::detail {

/** IOCP completion key values.

    These integer values are used as the completion key parameter
    when calling CreateIoCompletionPort and PostQueuedCompletionStatus.
    The run loop dispatches based on these values using a switch.

    All I/O handles are registered with key_io (0), and dispatch
    happens via the function pointer in the overlapped_op structure.
    The other keys are for internal scheduler signals.
*/
enum completion_key : ULONG_PTR
{
    /** I/O operation completed. OVERLAPPED* points to overlapped_op. */
    key_io = 0,

    /** Wakeup signal: timers are due, or work sits on the scheduler's
        shared queue. */
    key_wake_dispatch = 1,

    /** Scheduler stop/shutdown signal. */
    key_shutdown = 2,

    /** Operation completed with results pre-stored in OVERLAPPED fields.
        Used when posting completions after synchronous completion. */
    key_result_stored = 3,

    /** Posted scheduler_op*. OVERLAPPED* is actually a scheduler_op*. */
    key_posted = 4,

    /** Posted capy::continuation*. OVERLAPPED* is actually a
        capy::continuation*; resume its handle. */
    key_continuation = 5
};

namespace iocp {

// Why the scheduler's dispatch_required_ flag is raised: work sits on
// the shared queue, or the timer thread saw the earliest timer expire.
inline constexpr long dispatch_queue  = 1;
inline constexpr long dispatch_timers = 2;

} // namespace iocp

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_COMPLETION_KEY_HPP
