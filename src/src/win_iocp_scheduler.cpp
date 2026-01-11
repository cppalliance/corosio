//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/win_iocp_scheduler.hpp"

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

win_iocp_scheduler::
win_iocp_scheduler(
    capy::execution_context&)
    : iocp_(CreateIoCompletionPort(
        INVALID_HANDLE_VALUE,
        nullptr,
        0,
        0))
    , thread_id_(std::this_thread::get_id())
{
    if (iocp_ == nullptr)
    {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "CreateIoCompletionPort failed");
    }
}

win_iocp_scheduler::
~win_iocp_scheduler()
{
    if (iocp_ != nullptr)
    {
        CloseHandle(iocp_);
    }
}

void
win_iocp_scheduler::
shutdown()
{
    // Post a shutdown signal to wake any blocked threads
    ::PostQueuedCompletionStatus(
        iocp_,
        0,
        shutdown_key,
        nullptr);

    // Drain and destroy all pending work items
    DWORD bytes;
    ULONG_PTR key;
    LPOVERLAPPED overlapped;

    while (::GetQueuedCompletionStatus(
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
win_iocp_scheduler::
post(capy::coro h) const
{
    struct coro_work : capy::executor_work
    {
        capy::coro h_;

        explicit coro_work(capy::coro h)
            : h_(h)
        {
        }

        void operator()() override
        {
            // delete before dispatch to enable work recycling
            auto h = h_;
            delete this;
            h.resume();
        }

        void destroy() override
        {
            delete this;
        }
    };

    post(new coro_work(h));
}

void
win_iocp_scheduler::
post(capy::executor_work* w) const
{
    // Post the work item to the IOCP
    // We use the OVERLAPPED* field to carry the work pointer
    BOOL result = ::PostQueuedCompletionStatus(
        iocp_,
        0,
        work_key,
        reinterpret_cast<LPOVERLAPPED>(w));

    if (!result)
    {
        // If posting fails, destroy the work item
        w->destroy();

        // Claude: what should we do here?
    }
}

bool
win_iocp_scheduler::
running_in_this_thread() const noexcept
{
    return std::this_thread::get_id() == thread_id_;
}

void
win_iocp_scheduler::
stop()
{
    stopped_.store(true, std::memory_order_release);
    // Post a shutdown signal to wake any blocked threads
    ::PostQueuedCompletionStatus(
        iocp_,
        0,
        shutdown_key,
        nullptr);
}

bool
win_iocp_scheduler::
stopped() const noexcept
{
    return stopped_.load(std::memory_order_acquire);
}

void
win_iocp_scheduler::
restart()
{
    stopped_.store(false, std::memory_order_release);
}

std::size_t
win_iocp_scheduler::
do_run(unsigned long timeout, std::size_t max_handlers)
{
    std::size_t count = 0;
    DWORD bytes;
    ULONG_PTR key;
    LPOVERLAPPED overlapped;

    while (count < max_handlers && !stopped())
    {
        BOOL result = ::GetQueuedCompletionStatus(
            iocp_,
            &bytes,
            &key,
            &overlapped,
            timeout);

        if (!result)
        {
            // Timeout or error
            break;
        }

        if (key == shutdown_key)
        {
            // Shutdown signal received - re-post for other threads
            ::PostQueuedCompletionStatus(
                iocp_,
                0,
                shutdown_key,
                nullptr);
            break;
        }

        if (key == work_key && overlapped != nullptr)
        {
            (*reinterpret_cast<capy::executor_work*>(overlapped))();
            ++count;
        }

        // After first handler, switch to non-blocking for poll behavior
        if (timeout == 0)
            continue;
    }

    return count;
}

std::size_t
win_iocp_scheduler::
run()
{
    std::size_t total = 0;

    while (!stopped())
    {
        std::size_t n = do_run(INFINITE, static_cast<std::size_t>(-1));
        if (n == 0)
            break;
        total += n;
    }

    return total;
}

std::size_t
win_iocp_scheduler::
run_one()
{
    return do_run(INFINITE, 1);
}

std::size_t
win_iocp_scheduler::
run_for(std::chrono::steady_clock::duration rel_time)
{
    auto end_time = std::chrono::steady_clock::now() + rel_time;
    return run_until(end_time);
}

std::size_t
win_iocp_scheduler::
run_until(std::chrono::steady_clock::time_point abs_time)
{
    std::size_t total = 0;

    while (!stopped())
    {
        auto now = std::chrono::steady_clock::now();
        if (now >= abs_time)
            break;

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            abs_time - now);
        DWORD timeout = static_cast<DWORD>(remaining.count());
        if (timeout == 0)
            timeout = 1; // Minimum 1ms to avoid pure poll

        std::size_t n = do_run(timeout, static_cast<std::size_t>(-1));
        total += n;

        if (n == 0)
            break;
    }

    return total;
}

std::size_t
win_iocp_scheduler::
poll()
{
    return do_run(0, static_cast<std::size_t>(-1));
}

std::size_t
win_iocp_scheduler::
poll_one()
{
    return do_run(0, 1);
}

} // namespace corosio
} // namespace boost

#endif // _WIN32
