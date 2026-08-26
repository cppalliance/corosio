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
#include <boost/corosio/native/detail/iocp/win_wsa_init.hpp>

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
    acceptor-read, write-wait, error-wait), this reactor runs a
    dedicated thread using WSAPoll to detect readiness and posts a
    synthetic completion to the owning IOCP scheduler via
    win_scheduler::on_completion().

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

    The constructor builds the wakeup channel and throws if it cannot:
    a reactor that cannot be woken can never report readiness, so
    there is no reactor worth handing back. The polling thread is a
    separate cost, paid by the first register_wait, so a context that
    never waits never carries one.

    Thread-safe: register_wait, cancel_wait, and stop may be called
    from any thread.
*/
class win_wait_reactor : private win_wsa_init
{
public:
    /** Construct the reactor and its wakeup channel.

        @par Exception Safety
        Strong guarantee. A channel that cannot be formed leaves no
        socket open.

        @param sched The scheduler synthetic completions are posted to.

        @throws std::system_error If Winsock could not be started or
            the wakeup socket pair could not be built.
    */
    explicit win_wait_reactor(win_scheduler& sched);

    /// Stop the reactor thread and close the wakeup channel.
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
    DWORD queue_register(entry const& e);
    void wake_self() noexcept;
    DWORD make_wakeup_pair() noexcept;
    void close_wakeup_pair() noexcept;

    // A failed call that left a zero last error would answer "no
    // error" and put the reactor straight back on the silent path.
    static DWORD wakeup_error() noexcept
    {
        DWORD const err = ::WSAGetLastError();
        return err != 0 ? err : static_cast<DWORD>(WSAEINVAL);
    }

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

    // Built by the constructor and closed by the destructor, so every
    // other member function can assume a usable channel.
    SOCKET wakeup_read_  = INVALID_SOCKET;
    SOCKET wakeup_write_ = INVALID_SOCKET;

    // Also guards thread_ against a start racing the stop that joins it.
    std::mutex mutex_;
    std::vector<entry> pending_register_;
    std::vector<overlapped_op*> pending_cancel_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> wake_pending_{false};

    // Set by the polling thread on its way out, guarded by mutex_.
    // stop() is the ordinary way the thread leaves and has stop_ to
    // announce it; this covers the thread leaving on its own after a
    // WSAPoll error, which stop_ must not be used for -- stop() reads
    // it as "already stopped" and would skip the join that keeps the
    // thread from being destroyed joinable.
    bool dead_ = false;

    std::vector<entry> registered_; // reactor-thread-only

    std::thread thread_;
};

inline win_wait_reactor::win_wait_reactor(win_scheduler& sched)
    : sched_(sched)
{
    // The win_wsa_init base is what makes the sockets below legal, and
    // holding the reference rather than borrowing someone else's is
    // what keeps them legal to the end: a base is constructed before
    // this body and released after ~win_wait_reactor has closed the
    // pair, so WSACleanup can never land between the two.
    //
    // A reactor that cannot be woken would park every op it is handed
    // forever, so it refuses to exist rather than being handed out
    // broken. The polling thread waits for the first register_wait.
    if (DWORD const err = make_wakeup_pair(); err != 0)
        detail::throw_system_error(make_err(err), "win_wait_reactor");
}

inline win_wait_reactor::~win_wait_reactor()
{
    stop();
    close_wakeup_pair();
}

inline DWORD
win_wait_reactor::make_wakeup_pair() noexcept
{
    // Build a pair of connected loopback sockets to use as a wakeup
    // channel. Winsock has no socketpair(2), so we listen on
    // 127.0.0.1:0, connect a peer, then accept it.
    //
    // Every failure path reads the last error before closing
    // anything: closesocket() overwrites it.
    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
        return wakeup_error();

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
        DWORD const err = wakeup_error();
        ::closesocket(listener);
        return err;
    }

    wakeup_write_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (wakeup_write_ == INVALID_SOCKET)
    {
        DWORD const err = wakeup_error();
        ::closesocket(listener);
        return err;
    }

    if (::connect(
            wakeup_write_, reinterpret_cast<sockaddr*>(&addr), len) ==
        SOCKET_ERROR)
    {
        DWORD const err = wakeup_error();
        ::closesocket(wakeup_write_);
        wakeup_write_ = INVALID_SOCKET;
        ::closesocket(listener);
        return err;
    }

    wakeup_read_ = ::accept(listener, nullptr, nullptr);
    if (wakeup_read_ == INVALID_SOCKET)
    {
        DWORD const err = wakeup_error();
        ::closesocket(listener);
        ::closesocket(wakeup_write_);
        wakeup_write_ = INVALID_SOCKET;
        return err;
    }
    ::closesocket(listener);

    // The drain loop in run() calls recv() until it returns <= 0.
    // With a blocking socket that second recv() would block instead
    // of returning WSAEWOULDBLOCK, deadlocking the reactor thread.
    u_long non_blocking = 1;
    if (::ioctlsocket(wakeup_read_, FIONBIO, &non_blocking) == SOCKET_ERROR)
    {
        DWORD const err = wakeup_error();
        close_wakeup_pair();
        return err;
    }
    return 0;
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

    char b = 0;
    if (::send(wakeup_write_, &b, 1, 0) == SOCKET_ERROR)
    {
        // The flag is what coalesces later wakes into a byte already
        // in the channel; a send that failed put no byte there, so
        // leaving it latched would swallow every wake that follows.
        // Disarming keeps the cost to the wakes already in flight: the
        // next register, cancel or stop sends its own byte and the
        // reactor learns about both.
        wake_pending_.store(false, std::memory_order_release);
    }
}

inline void
win_wait_reactor::register_wait(
    SOCKET fd, wait_type w, overlapped_op* op)
{
    // If the op was already cancelled (e.g. pre-cancelled stop_token
    // fired synchronously before this call), complete immediately
    // instead of registering.  Otherwise the reactor would park the
    // op forever because the earlier cancel_wait() found nothing to
    // cancel in registered_.
    if (op->cancelled.load(std::memory_order_acquire))
    {
        sched_.on_completion(op, 0, 0);
        return;
    }

    if (DWORD const err = queue_register(entry{fd, w, op}); err != 0)
    {
        // The reactor is stopped, or its polling thread has died, so
        // nothing would ever drain a parked op. Report the abort its
        // own shutdown drain gives the ops it was still holding.
        sched_.on_completion(op, err, 0);
        return;
    }
    wake_self();
}

inline DWORD
win_wait_reactor::queue_register(entry const& e)
{
    std::lock_guard lock(mutex_);
    // stop() sets the flag before it takes the thread out from under
    // this lock, and never joins again. Checking the flag and queueing
    // in one critical section is what keeps both halves honest: a
    // thread started after that join would be destroyed still joinable,
    // which ends the process, and an op queued after it would have no
    // drainer. Queueing under the flag instead leaves the op for the
    // drain run() performs on its way out.
    // dead_ says the same thing for the other exit: a thread that left
    // on a WSAPoll error drained what it held and will not poll again,
    // so a register queued after it would wait on nobody.
    if (stop_.load(std::memory_order_acquire) || dead_)
        return ERROR_OPERATION_ABORTED;

    // A polling thread costs a thread per context, and a context that
    // never waits never pays for one; the first wait is what starts it.
    // A system that will not give one refuses this wait alone: nothing
    // has been queued yet, so the reactor is left exactly as it was and
    // the next register_wait asks again.
    if (!thread_.joinable())
    {
        try
        {
            thread_ = std::thread([this] { run(); });
        }
        catch (...)
        {
            return ERROR_MAX_THRDS_REACHED;
        }
    }

    pending_register_.push_back(e);
    return 0;
}

inline void
win_wait_reactor::cancel_wait(overlapped_op* op)
{
    {
        std::lock_guard lock(mutex_);
        // Same refusal queue_register makes, for the same reason: once
        // the polling thread is gone nothing reads pending_cancel_
        // again, so a cancel queued here would sit there for good. It
        // has nothing to cancel either -- the drain those exits run
        // already answered whatever was parked, and a register that
        // arrived after them was refused at its caller.
        if (stop_.load(std::memory_order_acquire) || dead_)
            return;
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
    // Moved out under the lock, then joined without it: the reactor
    // thread takes the same lock on every pass, so joining while
    // holding it would deadlock. A context that never waited has no
    // thread here at all.
    std::thread t;
    {
        std::lock_guard lock(mutex_);
        t = std::move(thread_);
    }
    if (t.joinable())
        t.join();
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
            // A socket's close() and cancel() ask the reactor to drop
            // their wait op whether or not one is parked -- open() goes
            // through close_socket() before it has a socket at all --
            // and such an ask can outlive the op's next reset().
            // Acting on it then would complete the wait that reset
            // started, the moment it is registered. Every real cancel
            // flags the op before queueing the ask, and only reset()
            // clears the flag, so an unflagged op is one of those
            // stale asks.
            if (!op->cancelled.load(std::memory_order_acquire))
                continue;

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

        // Block until the self-pipe (slot 0) is poked by a register,
        // cancel, or stop, or a watched socket becomes ready. No
        // periodic timeout, so an idle reactor consumes no CPU: the
        // self-pipe is the only thing that ends this wait, and
        // wake_self() leaves the channel free for the next poke when
        // its own send fails, so a lost wake costs the wakes already in
        // flight rather than every wake after it.
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
        // Closing the door and taking what is behind it in one critical
        // section is what leaves no register in between: one that got
        // in is drained here, one that arrives after is refused by
        // queue_register and completes as aborted at its caller.
        dead_ = true;
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
