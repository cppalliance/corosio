//
// Copyright (c) 2026 Cinar Gursoy
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_KQUEUE_OP_HPP
#define BOOST_COROSIO_DETAIL_KQUEUE_OP_HPP

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_KQUEUE)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io_object.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/concept/io_awaitable.hpp>
#include <boost/capy/coro.hpp>
#include <boost/capy/error.hpp>
#include <boost/system/error_code.hpp>

#include "src/detail/make_err.hpp"
#include "src/detail/scheduler_op.hpp"

#include <unistd.h>
#include <errno.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <boost/capy/ex/stop_token.hpp>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/event.h>

/*
    kqueue Operation State
    ======================

    Each async I/O operation has a corresponding kqueue_op-derived struct that
    holds the operation's state while it's in flight. The socket impl owns
    fixed slots for each operation type (conn_, rd_, wr_), so only one
    operation of each type can be pending per socket at a time.

    Completion vs Cancellation Race
    -------------------------------
    The `registered` atomic handles the race between kqueue signaling ready
    and cancel() being called. Whoever atomically exchanges it from true to
    false "claims" the operation and is responsible for completing it. The
    loser sees false and does nothing. This avoids double-completion bugs
    without requiring a mutex in the hot path.

    Impl Lifetime Management
    ------------------------
    When cancel() posts an op to the scheduler's ready queue, the socket impl
    might be destroyed before the scheduler processes the op. The `impl_ptr`
    member holds a shared_ptr to the impl, keeping it alive until the op
    completes. This is set by cancel() in sockets.hpp and cleared in operator()
    after the coroutine is resumed. Without this, closing a socket with pending
    operations causes use-after-free.

    EOF Detection
    -------------
    For reads, 0 bytes with no error means EOF. But an empty user buffer also
    returns 0 bytes. The `empty_buffer_read` flag distinguishes these cases
    so we don't spuriously report EOF when the user just passed an empty buffer.

    SIGPIPE Prevention
    ------------------
    On macOS, we use the SO_NOSIGPIPE socket option instead of MSG_NOSIGNAL
    (which doesn't exist on macOS). This is set when the socket is created.
*/

namespace boost {
namespace corosio {
namespace detail {

struct kqueue_op : scheduler_op
{
    struct canceller
    {
        kqueue_op* op;
        void operator()() const noexcept { op->request_cancel(); }
    };

    capy::coro h;
    capy::executor_ref d;
    system::error_code* ec_out = nullptr;
    std::size_t* bytes_out = nullptr;

    int fd = -1;
    int16_t filter = 0;  // EVFILT_READ or EVFILT_WRITE
    int errn = 0;
    std::size_t bytes_transferred = 0;

    std::atomic<bool> cancelled{false};
    std::atomic<bool> registered{false};
    std::optional<capy::stop_callback<canceller>> stop_cb;

    // Prevents use-after-free when socket is closed with pending ops.
    // See "Impl Lifetime Management" in file header.
    std::shared_ptr<void> impl_ptr;

    kqueue_op()
    {
        data_ = this;
    }

    void reset() noexcept
    {
        fd = -1;
        filter = 0;
        errn = 0;
        bytes_transferred = 0;
        cancelled.store(false, std::memory_order_relaxed);
        registered.store(false, std::memory_order_relaxed);
        impl_ptr.reset();
    }

    void operator()() override
    {
        stop_cb.reset();

        if (ec_out)
        {
            if (cancelled.load(std::memory_order_acquire))
                *ec_out = capy::error::canceled;
            else if (errn != 0)
                *ec_out = make_err(errn);
            else if (is_read_operation() && bytes_transferred == 0)
                *ec_out = capy::error::eof;
        }

        if (bytes_out)
            *bytes_out = bytes_transferred;

        auto saved_d = d;
        auto saved_h = std::move(h);
        impl_ptr.reset();
        saved_d.dispatch(saved_h).resume();
    }

    virtual bool is_read_operation() const noexcept { return false; }

    void destroy() override
    {
        stop_cb.reset();
        impl_ptr.reset();
    }

    void request_cancel() noexcept
    {
        cancelled.store(true, std::memory_order_release);
    }

    void start(capy::stop_token token)
    {
        cancelled.store(false, std::memory_order_release);
        stop_cb.reset();

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

inline kqueue_op*
get_kqueue_op(scheduler_op* h) noexcept
{
    return static_cast<kqueue_op*>(h->data());
}

//------------------------------------------------------------------------------

struct kqueue_connect_op : kqueue_op
{
    void perform_io() noexcept override
    {
        // connect() completion status is retrieved via SO_ERROR, not return value
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
            err = errno;
        complete(err, 0);
    }
};

//------------------------------------------------------------------------------

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
};

//------------------------------------------------------------------------------

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
        // On macOS, we use SO_NOSIGPIPE socket option instead of MSG_NOSIGNAL
        // The socket option is set when the socket is created
        ssize_t n = ::writev(fd, iovecs, iovec_count);
        if (n >= 0)
            complete(0, static_cast<std::size_t>(n));
        else
            complete(errno, 0);
    }
};

//------------------------------------------------------------------------------

struct kqueue_accept_op : kqueue_op
{
    int accepted_fd = -1;
    io_object::io_object_impl* peer_impl = nullptr;
    io_object::io_object_impl** impl_out = nullptr;

    using create_peer_fn = io_object::io_object_impl* (*)(void*, int);
    create_peer_fn create_peer = nullptr;
    void* service_ptr = nullptr;

    void reset() noexcept
    {
        kqueue_op::reset();
        accepted_fd = -1;
        peer_impl = nullptr;
        impl_out = nullptr;
    }

    void perform_io() noexcept override
    {
        sockaddr_in addr{};
        socklen_t addrlen = sizeof(addr);
        // macOS doesn't have accept4, use accept and set flags via fcntl
        int new_fd = ::accept(fd, reinterpret_cast<sockaddr*>(&addr), &addrlen);

        if (new_fd >= 0)
        {
            // Set non-blocking
            int flags = ::fcntl(new_fd, F_GETFL, 0);
            if (flags >= 0)
                ::fcntl(new_fd, F_SETFL, flags | O_NONBLOCK);

            // Set close-on-exec
            flags = ::fcntl(new_fd, F_GETFD, 0);
            if (flags >= 0)
                ::fcntl(new_fd, F_SETFD, flags | FD_CLOEXEC);

            // Set SO_NOSIGPIPE to prevent SIGPIPE on write to closed socket
            int nosigpipe = 1;
            ::setsockopt(new_fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));

            accepted_fd = new_fd;
            if (create_peer && service_ptr)
                peer_impl = create_peer(service_ptr, new_fd);
            complete(0, 0);
        }
        else
        {
            complete(errno, 0);
        }
    }

    void operator()() override
    {
        stop_cb.reset();

        bool success = (errn == 0 && !cancelled.load(std::memory_order_acquire));

        if (ec_out)
        {
            if (cancelled.load(std::memory_order_acquire))
                *ec_out = capy::error::canceled;
            else if (errn != 0)
                *ec_out = make_err(errn);
        }

        if (success && accepted_fd >= 0 && peer_impl)
        {
            if (impl_out)
                *impl_out = peer_impl;
            peer_impl = nullptr;
        }
        else
        {
            if (accepted_fd >= 0)
            {
                ::close(accepted_fd);
                accepted_fd = -1;
            }

            if (peer_impl)
            {
                peer_impl->release();
                peer_impl = nullptr;
            }

            if (impl_out)
                *impl_out = nullptr;
        }

        auto saved_d = d;
        auto saved_h = std::move(h);
        impl_ptr.reset();
        saved_d.dispatch(saved_h).resume();
    }
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // BOOST_COROSIO_BACKEND_KQUEUE

#endif // BOOST_COROSIO_DETAIL_KQUEUE_OP_HPP
