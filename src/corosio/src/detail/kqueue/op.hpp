//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_KQUEUE_OP_HPP
#define BOOST_COROSIO_DETAIL_KQUEUE_OP_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io_object.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <coroutine>
#include <boost/capy/error.hpp>
#include <system_error>

#include "src/detail/scheduler_op.hpp"

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

/*
    kqueue Operation State
    ======================

    Each async I/O operation has a corresponding kqueue_op-derived struct that
    holds the operation's state while it's in flight. The socket impl owns
    fixed slots for each operation type (conn_, rd_, wr_), so only one
    operation of each type can be pending per socket at a time.

    Persistent Registration
    -----------------------
    File descriptors are registered with kqueue once (via descriptor_state) and
    stay registered until closed. Uses EV_CLEAR for edge-triggered semantics
    (equivalent to epoll's EPOLLET). The descriptor_state tracks which operations
    are pending (read_op, write_op, connect_op). When an event arrives, the
    reactor dispatches to the appropriate pending operation.

    Impl Lifetime Management
    ------------------------
    When cancel() posts an op to the scheduler's ready queue, the socket impl
    might be destroyed before the scheduler processes the op. The `impl_ptr`
    member holds a shared_ptr to the impl, keeping it alive until the op
    completes. This is set by cancel() and cleared in operator() after the
    coroutine is resumed.

    EOF Detection
    -------------
    For reads, 0 bytes with no error means EOF. But an empty user buffer also
    returns 0 bytes. The `empty_buffer_read` flag distinguishes these cases.

    SIGPIPE Prevention
    ------------------
    SO_NOSIGPIPE is set on each socket at creation time (see sockets.cpp).
    Writes use writev() which is safe because the socket-level option suppresses
    SIGPIPE delivery.
*/

namespace boost::corosio::detail {

// Ready-event flag constants for descriptor_state::ready_events_.
// These match the epoll numeric values (EPOLLIN=0x1, EPOLLOUT=0x4,
// EPOLLERR=0x8) so that descriptor_state::operator()() uses the same
// flag-checking logic as the epoll backend.
static constexpr std::uint32_t kqueue_event_read  = 0x001;
static constexpr std::uint32_t kqueue_event_write = 0x004;
static constexpr std::uint32_t kqueue_event_error = 0x008;

// Forward declarations
class kqueue_socket_impl;
class kqueue_acceptor_impl;
struct kqueue_op;

class kqueue_scheduler;

/** Per-descriptor state for persistent kqueue registration.

    Tracks pending operations for a file descriptor. The fd is registered
    once with kqueue (EVFILT_READ + EVFILT_WRITE, both EV_CLEAR) and stays
    registered until closed.

    This struct extends scheduler_op to support deferred I/O processing.
    When kqueue events arrive, the reactor sets ready_events and queues
    this descriptor for processing. When popped from the scheduler queue,
    operator() performs the actual I/O and queues completion handlers.

    @par Deferred I/O Model
    The reactor no longer performs I/O directly. Instead:
    1. Reactor sets ready_events and queues descriptor_state
    2. Scheduler pops descriptor_state and calls operator()
    3. operator() performs I/O under mutex and queues completions

    This eliminates per-descriptor mutex locking from the reactor hot path.

    @par Thread Safety
    The mutex protects operation pointers and ready flags during I/O.
    ready_events_ and is_enqueued_ are atomic for lock-free reactor access.
*/
struct descriptor_state : scheduler_op
{
    std::mutex mutex;

    // Protected by mutex
    kqueue_op* read_op = nullptr;
    kqueue_op* write_op = nullptr;
    kqueue_op* connect_op = nullptr;

    // Caches edge events that arrived before an op was registered
    bool read_ready = false;
    bool write_ready = false;

    // Deferred cancellation: set by cancel() when the target op is not
    // parked (e.g. completing inline via speculative I/O). Checked when
    // the next op parks; if set, the op is immediately self-cancelled.
    // This matches IOCP semantics where CancelIoEx always succeeds.
    bool read_cancel_pending = false;
    bool write_cancel_pending = false;
    bool connect_cancel_pending = false;

    // Set during registration only (no mutex needed)
    std::uint32_t registered_events = 0;
    int fd = -1;

    // For deferred I/O - set by reactor, read by scheduler
    std::atomic<std::uint32_t> ready_events_{0};
    std::atomic<bool> is_enqueued_{false};
    kqueue_scheduler const* scheduler_ = nullptr;

    // Prevents impl destruction while this descriptor_state is queued.
    // Set by close_socket() when is_enqueued_ is true, cleared by operator().
    std::shared_ptr<void> impl_ref_;

    /// Add ready events atomically.
    /// Release pairs with the consumer's acquire exchange on
    /// ready_events_ so the consumer sees all flags. On x86 (TSO)
    /// this compiles to the same LOCK OR as relaxed.
    void add_ready_events(std::uint32_t ev) noexcept
    {
        ready_events_.fetch_or(ev, std::memory_order_release);
    }

    /// Perform deferred I/O and queue completions.
    void operator()() override;

    /// Destroy without invoking.
    /// Called during scheduler::shutdown() drain. Clear impl_ref_ to break
    /// the self-referential cycle set by close_socket().
    void destroy() override { impl_ref_.reset(); }
};

struct kqueue_op : scheduler_op
{
    struct canceller
    {
        kqueue_op* op;
        void operator()() const noexcept;
    };

    std::coroutine_handle<> h;
    capy::executor_ref ex;
    std::error_code* ec_out = nullptr;
    std::size_t* bytes_out = nullptr;

    int fd = -1;
    int errn = 0;
    std::size_t bytes_transferred = 0;

    std::atomic<bool> cancelled{false};
    std::optional<std::stop_callback<canceller>> stop_cb;

    // Prevents use-after-free when socket is closed with pending ops.
    // See "Impl Lifetime Management" in file header.
    std::shared_ptr<void> impl_ptr;

    // For stop_token cancellation - pointer to owning socket/acceptor impl.
    // When stop is requested, we call back to the impl to perform actual I/O cancellation.
    kqueue_socket_impl* socket_impl_ = nullptr;
    kqueue_acceptor_impl* acceptor_impl_ = nullptr;

    kqueue_op() = default;

    void reset() noexcept
    {
        fd = -1;
        errn = 0;
        bytes_transferred = 0;
        cancelled.store(false, std::memory_order_relaxed);
        impl_ptr.reset();
        socket_impl_ = nullptr;
        acceptor_impl_ = nullptr;
    }

    // Defined in sockets.cpp where kqueue_socket_impl is complete
    void operator()() override;

    virtual bool is_read_operation() const noexcept { return false; }
    virtual void cancel() noexcept = 0;

    void destroy() override
    {
        stop_cb.reset();
        impl_ptr.reset();
    }

    void request_cancel() noexcept
    {
        cancelled.store(true, std::memory_order_release);
    }

    void start(std::stop_token token, kqueue_socket_impl* impl)
    {
        cancelled.store(false, std::memory_order_release);
        stop_cb.reset();
        socket_impl_ = impl;
        acceptor_impl_ = nullptr;

        if (token.stop_possible())
            stop_cb.emplace(token, canceller{this});
    }

    void start(std::stop_token token, kqueue_acceptor_impl* impl)
    {
        cancelled.store(false, std::memory_order_release);
        stop_cb.reset();
        socket_impl_ = nullptr;
        acceptor_impl_ = impl;

        if (token.stop_possible())
            stop_cb.emplace(token, canceller{this});
    }

    void complete(int err, std::size_t bytes) noexcept
    {
        errn = err;
        bytes_transferred = bytes;
    }

    virtual void perform_io() noexcept {}
};


struct kqueue_connect_op : kqueue_op
{
    endpoint target_endpoint;

    void reset() noexcept
    {
        kqueue_op::reset();
        target_endpoint = endpoint{};
    }

    void perform_io() noexcept override
    {
        // connect() completion status is retrieved via SO_ERROR, not return value
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
            err = errno;
        complete(err, 0);
    }

    // Defined in sockets.cpp where kqueue_socket_impl is complete
    void operator()() override;
    void cancel() noexcept override;
};


struct kqueue_read_op : kqueue_op
{
    static constexpr std::size_t max_buffers = 16;
    iovec iovecs[max_buffers];
    int iovec_count = 0;
    bool empty_buffer_read = false;

    bool is_read_operation() const noexcept override
    {
        return !empty_buffer_read;
    }

    void reset() noexcept
    {
        kqueue_op::reset();
        iovec_count = 0;
        empty_buffer_read = false;
    }

    void perform_io() noexcept override
    {
        ssize_t n = ::readv(fd, iovecs, iovec_count);
        if (n >= 0)
            complete(0, static_cast<std::size_t>(n));
        else
            complete(errno, 0);
    }

    void cancel() noexcept override;
};


struct kqueue_write_op : kqueue_op
{
    static constexpr std::size_t max_buffers = 16;
    iovec iovecs[max_buffers];
    int iovec_count = 0;

    void reset() noexcept
    {
        kqueue_op::reset();
        iovec_count = 0;
    }

    void perform_io() noexcept override
    {
        // SO_NOSIGPIPE is set on the socket at creation time (see sockets.cpp),
        // so writev() is safe from SIGPIPE.
        // FreeBSD: Supports MSG_NOSIGNAL on sendmsg()
        ssize_t n = ::writev(fd, iovecs, iovec_count);
        if (n >= 0)
            complete(0, static_cast<std::size_t>(n));
        else
            complete(errno, 0);
    }

    void cancel() noexcept override;
};


struct kqueue_accept_op : kqueue_op
{
    int accepted_fd = -1;
    io_object::io_object_impl* peer_impl = nullptr;
    io_object::io_object_impl** impl_out = nullptr;

    void reset() noexcept
    {
        kqueue_op::reset();
        accepted_fd = -1;
        peer_impl = nullptr;
        impl_out = nullptr;
    }

    void perform_io() noexcept override
    {
        sockaddr_storage addr_storage{};
        socklen_t addrlen = sizeof(addr_storage);

        // FreeBSD: Can use accept4(fd, addr, len, SOCK_NONBLOCK | SOCK_CLOEXEC)
        int new_fd = ::accept(fd, reinterpret_cast<sockaddr*>(&addr_storage), &addrlen);

        if (new_fd >= 0)
        {
            // Set non-blocking
            int flags = ::fcntl(new_fd, F_GETFL, 0);
            if (flags == -1 || ::fcntl(new_fd, F_SETFL, flags | O_NONBLOCK) == -1)
            {
                int err = errno;
                ::close(new_fd);
                complete(err, 0);
                return;
            }

            // Set close-on-exec
            if (::fcntl(new_fd, F_SETFD, FD_CLOEXEC) == -1)
            {
                int err = errno;
                ::close(new_fd);
                complete(err, 0);
                return;
            }

            // Suppress SIGPIPE on accepted sockets; macOS lacks MSG_NOSIGNAL
            int one = 1;
            if (::setsockopt(new_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) == -1)
            {
                int err = errno;
                ::close(new_fd);
                complete(err, 0);
                return;
            }

            accepted_fd = new_fd;
            complete(0, 0);
        }
        else
        {
            complete(errno, 0);
        }
    }

    // Defined in acceptors.cpp where kqueue_acceptor_impl is complete
    void operator()() override;
    void cancel() noexcept override;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_DETAIL_KQUEUE_OP_HPP
