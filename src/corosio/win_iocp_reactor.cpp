//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/win_iocp_reactor.hpp>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <system_error>

namespace boost {
namespace corosio {

namespace {

// Completion key used to identify work items
constexpr ULONG_PTR work_key = 1;

// Completion key used to signal shutdown
constexpr ULONG_PTR shutdown_key = 0;

} // namespace

win_iocp_reactor::
win_iocp_reactor(capy::service_provider&)
    : iocp_(CreateIoCompletionPort(
        INVALID_HANDLE_VALUE,
        nullptr,
        0,
        0))
{
    if (iocp_ == nullptr)
    {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "CreateIoCompletionPort failed");
    }
}

win_iocp_reactor::
~win_iocp_reactor()
{
    if (iocp_ != nullptr)
    {
        CloseHandle(iocp_);
    }
}

void
win_iocp_reactor::
shutdown()
{
    // Post a shutdown signal to wake any blocked threads
    PostQueuedCompletionStatus(
        iocp_,
        0,
        shutdown_key,
        nullptr);

    // Drain and destroy all pending work items
    DWORD bytes;
    ULONG_PTR key;
    LPOVERLAPPED overlapped;

    while (GetQueuedCompletionStatus(
        iocp_,
        &bytes,
        &key,
        &overlapped,
        0)) // Non-blocking
    {
        if (key == work_key && overlapped != nullptr)
        {
            auto* work = reinterpret_cast<capy::executor_work*>(overlapped);
            work->destroy();
        }
        // Ignore shutdown signals during drain
    }
}

void
win_iocp_reactor::
submit(capy::executor_work* w)
{
    // Post the work item to the IOCP
    // We use the OVERLAPPED* field to carry the work pointer
    BOOL result = PostQueuedCompletionStatus(
        iocp_,
        0,
        work_key,
        reinterpret_cast<LPOVERLAPPED>(w));

    if (!result)
    {
        // If posting fails, destroy the work item
        w->destroy();
    }
}

void
win_iocp_reactor::
process()
{
    DWORD bytes;
    ULONG_PTR key;
    LPOVERLAPPED overlapped;

    // Process all available completions without blocking
    while (GetQueuedCompletionStatus(
        iocp_,
        &bytes,
        &key,
        &overlapped,
        0)) // Timeout of 0 = non-blocking
    {
        if (key == shutdown_key)
        {
            // Shutdown signal received
            break;
        }

        if (key == work_key && overlapped != nullptr)
        {
            auto* work = reinterpret_cast<capy::executor_work*>(overlapped);

            // Execute the work item
            // The work item is responsible for deleting itself
            (*work)();
        }
    }
}

} // namespace corosio
} // namespace boost

#endif // _WIN32
