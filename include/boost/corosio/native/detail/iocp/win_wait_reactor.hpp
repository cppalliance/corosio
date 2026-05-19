//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_WAIT_REACTOR_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_WAIT_REACTOR_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

// This header is included from the bottom of win_scheduler.hpp after
// the scheduler class is fully defined. Including it directly would
// circle back into a still-incomplete win_scheduler when the dtor's
// unique_ptr<win_wait_reactor>::reset() is parsed. Diagnose that
// rather than emitting a confusing "incomplete type" error far away.
#ifndef BOOST_COROSIO_DETAIL_IOCP_WIN_SCHEDULER_BODY_DONE
#error "Include <boost/corosio/native/detail/iocp/win_scheduler.hpp> \
instead of including this header directly."
#endif

#include <boost/corosio/wait_type.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <Ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace boost::corosio::detail {

/** Auxiliary select-based reactor for IOCP wait operations.

    IOCP has no native primitive for socket readiness without I/O.
    For cases where a zero-byte WSARecv won't work (datagram-read,
    acceptor-read, error-wait), this reactor runs a dedicated thread
    using WSAPoll to detect readiness and posts a synthetic completion
    to the owning IOCP scheduler via win_scheduler::on_completion().

    The same dispatch path used by overlapped I/O then delivers the
    completion to the user's coroutine, so the public API is uniform
    across backends.

    Per-op lifecycle:
    1. Caller sets up an overlapped_op (h, ex, ec_out, cancelled flag).
    2. Caller calls register_wait(fd, w, op) and returns
       std::noop_coroutine. The op is parked in the reactor's table.
    3. Reactor thread polls. When the fd is ready, the op is removed
       from the table and posted to the scheduler. The error code
       delivered to the completion is: ec={} on success; the SO_ERROR
       value if error revents fired and SO_ERROR is set; or
       WSAECONNABORTED as a synthesized fallback for wait_type::error
       when error revents fired but SO_ERROR returned zero.
    4. On socket cancel(), the user's thread calls cancel_wait(op),
       which queues a cancel request. The reactor thread removes the
       op from the table and posts a completion; invoke_handler sees
       op.cancelled==true and yields capy::cond::canceled.

    Thread-safe: register_wait, cancel_wait, and stop may be called
    from any thread.
*/
class win_wait_reactor
{
public:
    explicit win_wait_reactor(win_scheduler& sched);
    ~win_wait_reactor();

    win_wait_reactor(win_wait_reactor const&)            = delete;
    win_wait_reactor& operator=(win_wait_reactor const&) = delete;

    /// Park an overlapped_op until @p fd is ready for @p w.
    void register_wait(SOCKET fd, wait_type w, overlapped_op* op);

    /// Remove a parked op and post a completion. Idempotent.
    void cancel_wait(overlapped_op* op);

    /// Stop the reactor thread and drain remaining ops as cancelled.
    void stop();

private:
    struct entry
    {
        SOCKET fd         = INVALID_SOCKET;
        wait_type w       = wait_type::read;
        overlapped_op* op = nullptr;
    };

    void run();
    void wake_self() noexcept;
    void make_wakeup_pair();
    void close_wakeup_pair() noexcept;

    static SHORT events_for_wait(wait_type w) noexcept
    {
        switch (w)
        {
        case wait_type::read:  return POLLRDNORM;
        case wait_type::write: return POLLWRNORM;
        default:               return POLLPRI;
        }
    }

    static bool ready_for_wait(wait_type w, SHORT revents) noexcept
    {
        constexpr SHORT err_bits = POLLERR | POLLHUP | POLLNVAL;
        switch (w)
        {
        case wait_type::read:
            return (revents & (POLLRDNORM | POLLRDBAND | err_bits)) != 0;
        case wait_type::write:
            return (revents & (POLLWRNORM | POLLWRBAND | err_bits)) != 0;
        default:
            return (revents & (POLLPRI | err_bits)) != 0;
        }
    }

    win_scheduler& sched_;

    SOCKET wakeup_read_  = INVALID_SOCKET;
    SOCKET wakeup_write_ = INVALID_SOCKET;

    std::mutex mutex_;
    std::vector<entry> pending_register_;
    std::vector<overlapped_op*> pending_cancel_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> wake_pending_{false};

    std::vector<entry> registered_; // reactor-thread-only

    std::thread thread_;
};

inline win_wait_reactor::win_wait_reactor(win_scheduler& sched)
    : sched_(sched)
{
    make_wakeup_pair();
    thread_ = std::thread([this] { run(); });
}

inline win_wait_reactor::~win_wait_reactor()
{
    stop();
    close_wakeup_pair();
}

inline void
win_wait_reactor::make_wakeup_pair()
{
    // Build a pair of connected loopback sockets to use as a wakeup
    // channel. Winsock has no socketpair(2), so we listen on
    // 127.0.0.1:0, connect a peer, then accept it.
    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
        return;

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    int len = sizeof(addr);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), len) ==
            SOCKET_ERROR ||
        ::listen(listener, 1) == SOCKET_ERROR ||
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) ==
            SOCKET_ERROR)
    {
        ::closesocket(listener);
        return;
    }

    wakeup_write_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (wakeup_write_ == INVALID_SOCKET)
    {
        ::closesocket(listener);
        return;
    }

    if (::connect(
            wakeup_write_, reinterpret_cast<sockaddr*>(&addr), len) ==
        SOCKET_ERROR)
    {
        ::closesocket(wakeup_write_);
        wakeup_write_ = INVALID_SOCKET;
        ::closesocket(listener);
        return;
    }

    wakeup_read_ = ::accept(listener, nullptr, nullptr);
    ::closesocket(listener);

    if (wakeup_read_ == INVALID_SOCKET)
    {
        ::closesocket(wakeup_write_);
        wakeup_write_ = INVALID_SOCKET;
        return;
    }

    // The drain loop in run() calls recv() until it returns <= 0.
    // With a blocking socket that second recv() would block instead
    // of returning WSAEWOULDBLOCK, deadlocking the reactor thread.
    u_long non_blocking = 1;
    ::ioctlsocket(wakeup_read_, FIONBIO, &non_blocking);
}

inline void
win_wait_reactor::close_wakeup_pair() noexcept
{
    if (wakeup_read_ != INVALID_SOCKET)
    {
        ::closesocket(wakeup_read_);
        wakeup_read_ = INVALID_SOCKET;
    }
    if (wakeup_write_ != INVALID_SOCKET)
    {
        ::closesocket(wakeup_write_);
        wakeup_write_ = INVALID_SOCKET;
    }
}

inline void
win_wait_reactor::wake_self() noexcept
{
    // Coalesce wakes: only send a byte if no wake is already pending.
    bool expected = false;
    if (!wake_pending_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
        return;
    if (wakeup_write_ != INVALID_SOCKET)
    {
        char b = 0;
        ::send(wakeup_write_, &b, 1, 0);
    }
}

inline void
win_wait_reactor::register_wait(
    SOCKET fd, wait_type w, overlapped_op* op)
{
    {
        std::lock_guard lock(mutex_);
        pending_register_.push_back(entry{fd, w, op});
    }
    wake_self();
}

inline void
win_wait_reactor::cancel_wait(overlapped_op* op)
{
    {
        std::lock_guard lock(mutex_);
        pending_cancel_.push_back(op);
    }
    wake_self();
}

inline void
win_wait_reactor::stop()
{
    if (stop_.exchange(true, std::memory_order_acq_rel))
        return;
    wake_self();
    if (thread_.joinable())
        thread_.join();
}

inline void
win_wait_reactor::run()
{
    std::vector<WSAPOLLFD> pollfds;

    while (!stop_.load(std::memory_order_acquire))
    {
        // Drain pending register/cancel under the lock.
        std::vector<entry> to_add;
        std::vector<overlapped_op*> to_cancel;
        {
            std::lock_guard lock(mutex_);
            to_add.swap(pending_register_);
            to_cancel.swap(pending_cancel_);
        }

        for (auto& e : to_add)
            registered_.push_back(e);

        for (auto* op : to_cancel)
        {
            auto it = std::find_if(
                registered_.begin(), registered_.end(),
                [op](entry const& e) { return e.op == op; });
            if (it != registered_.end())
            {
                // The op's cancelled flag has already been set by
                // request_cancel; invoke_handler will translate it.
                sched_.on_completion(op, 0, 0);
                registered_.erase(it);
            }
            // If not in registered_, the op already fired — no-op.
        }

        // Build the poll set. Slot 0 is the wakeup socket.
        pollfds.clear();
        pollfds.reserve(registered_.size() + 1);
        pollfds.push_back({wakeup_read_, POLLRDNORM, 0});
        for (auto& e : registered_)
            pollfds.push_back({e.fd, events_for_wait(e.w), 0});

        int n = ::WSAPoll(
            pollfds.data(),
            static_cast<ULONG>(pollfds.size()),
            -1 /* infinite */);
        if (n == SOCKET_ERROR)
            break;

        // Drain the wakeup socket so it stops reporting readable.
        if (pollfds[0].revents != 0)
        {
            char buf[64];
            for (;;)
            {
                int r = ::recv(wakeup_read_, buf, sizeof(buf), 0);
                if (r <= 0)
                    break;
            }
            wake_pending_.store(false, std::memory_order_release);
        }

        // Walk events in reverse so erases don't invalidate later indices.
        for (std::size_t i = pollfds.size(); i > 1; --i)
        {
            auto const& pfd = pollfds[i - 1];
            if (pfd.revents == 0)
                continue;

            auto const& e = registered_[i - 2];
            if (!ready_for_wait(e.w, pfd.revents))
                continue;

            DWORD err = 0;
            constexpr SHORT err_bits = POLLERR | POLLHUP | POLLNVAL;
            if (pfd.revents & err_bits)
            {
                int so_err = 0;
                int sz     = sizeof(so_err);
                if (::getsockopt(
                        e.fd, SOL_SOCKET, SO_ERROR,
                        reinterpret_cast<char*>(&so_err), &sz) == 0 &&
                    so_err != 0)
                {
                    err = static_cast<DWORD>(so_err);
                }
                else if (e.w == wait_type::error)
                {
                    // wait_type::error fires on the error condition;
                    // the contract is to report a non-zero error_code.
                    err = WSAECONNABORTED;
                }
            }

            sched_.on_completion(e.op, err, 0);
            registered_.erase(registered_.begin() + (i - 2));
        }
    }

    // Drain remaining ops as cancelled on shutdown. This must cover
    // both the active set and anything still queued by user threads
    // that hasn't been moved into registered_ yet, otherwise those
    // ops leak work_started credit and stall scheduler shutdown.
    {
        std::lock_guard lock(mutex_);
        for (auto& e : pending_register_)
            registered_.push_back(e);
        pending_register_.clear();
        pending_cancel_.clear();
    }
    for (auto& e : registered_)
        sched_.on_completion(e.op, ERROR_OPERATION_ABORTED, 0);
    registered_.clear();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_WAIT_REACTOR_HPP
