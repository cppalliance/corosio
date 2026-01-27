//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_IOCP_COMPLETION_KEY_HPP
#define BOOST_COROSIO_DETAIL_IOCP_COMPLETION_KEY_HPP

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_IOCP)

#include "src/detail/iocp/windows.hpp"

namespace boost::corosio::detail {

class win_scheduler;

/** Abstract base for IOCP completion key dispatch.

    Each subsystem that posts completions to the IOCP owns a key
    object derived from this class. The key's address is used as
    the completion key value, enabling polymorphic dispatch when
    completions are dequeued.
*/
struct completion_key
{
    /** Result of completion handling, controls run loop behavior. */
    enum class result
    {
        did_work,       // Handler was invoked, count as work done
        continue_loop,  // No work done, continue polling
        stop_loop       // Stop the run loop
    };

    /** Handle a completion from the IOCP.

        @param sched The scheduler dequeuing the completion.
        @param bytes Bytes transferred (from GQCS).
        @param dwError Error code (from GetLastError if GQCS failed).
        @param overlapped The OVERLAPPED pointer (may be nullptr for signals).
        @return Action for the run loop to take.
    */
    virtual result on_completion(
        win_scheduler& sched,
        DWORD bytes,
        DWORD dwError,
        LPOVERLAPPED overlapped) = 0;

    /** Destroy a completion during shutdown without invoking handler.

        @param overlapped The OVERLAPPED pointer to destroy.
    */
    virtual void destroy(LPOVERLAPPED /*overlapped*/) {}

    /** Re-queue this key to the IOCP.

        Allows a key to post itself back to the completion port,
        e.g., to propagate signals to other waiting threads.

        @param iocp The completion port handle.
        @param overlapped Optional OVERLAPPED pointer (default nullptr).
    */
    void repost(void* iocp, LPOVERLAPPED overlapped = nullptr)
    {
        ::PostQueuedCompletionStatus(
            static_cast<HANDLE>(iocp),
            0,
            reinterpret_cast<ULONG_PTR>(this),
            overlapped);
    }

    virtual ~completion_key() = default;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_BACKEND_IOCP

#endif // BOOST_COROSIO_DETAIL_IOCP_COMPLETION_KEY_HPP
