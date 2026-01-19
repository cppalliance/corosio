//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_POSIX_OP_HPP
#define BOOST_COROSIO_DETAIL_POSIX_OP_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io_object.hpp>
#include <boost/capy/ex/any_executor_ref.hpp>
#include <boost/capy/concept/io_awaitable.hpp>
#include <boost/capy/ex/any_coro.hpp>
#include <boost/capy/error.hpp>
#include <boost/system/error_code.hpp>

#include "src/detail/scheduler_op.hpp"

#include <unistd.h>
#include <errno.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

namespace boost {
namespace corosio {
namespace detail {

/** Base class for POSIX async operations.

    This class is analogous to overlapped_op on Windows.
    It stores the coroutine handle, executor, and result
    pointers needed to complete an async operation.
*/
struct posix_op : scheduler_op
{
    struct canceller
    {
        posix_op* op;
        void operator()() const noexcept { op->request_cancel(); }
    };

    capy::any_coro h;
    capy::any_executor_ref d;
    system::error_code* ec_out = nullptr;
    std::size_t* bytes_out = nullptr;

    int fd = -1;
    std::uint32_t events = 0;
    int error = 0;
    std::size_t bytes_transferred = 0;

    std::atomic<bool> cancelled{false};
    std::optional<std::stop_callback<canceller>> stop_cb;

    posix_op()
    {
        data_ = this;
    }

    void reset() noexcept
    {
        fd = -1;
        events = 0;
        error = 0;
        bytes_transferred = 0;
        cancelled.store(false, std::memory_order_relaxed);
    }

    void operator()() override
    {
        stop_cb.reset();

        if (ec_out)
        {
            if (cancelled.load(std::memory_order_acquire))
                *ec_out = make_error_code(system::errc::operation_canceled);
            else if (error != 0)
                *ec_out = system::error_code(error, system::system_category());
            else if (is_read_operation() && bytes_transferred == 0)
            {
                // EOF: 0 bytes transferred with no error indicates end of stream
                *ec_out = make_error_code(capy::error::eof);
            }
        }

        if (bytes_out)
            *bytes_out = bytes_transferred;

        d.dispatch(h).resume();
    }

    virtual bool is_read_operation() const noexcept { return false; }

    void destroy() override
    {
        stop_cb.reset();
    }

    void request_cancel() noexcept
    {
        cancelled.store(true, std::memory_order_release);
    }

    void start(std::stop_token token)
    {
        cancelled.store(false, std::memory_order_release);
        stop_cb.reset();

        if (token.stop_possible())
            stop_cb.emplace(token, canceller{this});
    }

    void complete(int err, std::size_t bytes) noexcept
    {
        error = err;
        bytes_transferred = bytes;
    }

    /** Called when epoll signals the fd is ready.
        Derived classes override this to perform the actual I/O.
        Sets error and bytes_transferred appropriately.
    */
    virtual void perform_io() noexcept {}
};

inline posix_op*
get_posix_op(scheduler_op* h) noexcept
{
    return static_cast<posix_op*>(h->data());
}

//------------------------------------------------------------------------------

/** Connect operation state. */
struct posix_connect_op : posix_op
{
    void perform_io() noexcept override
    {
        // For connect, check SO_ERROR to see if connection succeeded
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
            err = errno;
        complete(err, 0);
    }
};

//------------------------------------------------------------------------------

/** Read operation state with buffer descriptors. */
struct posix_read_op : posix_op
{
    static constexpr std::size_t max_buffers = 16;
    iovec iovecs[max_buffers];
    int iovec_count = 0;

    bool is_read_operation() const noexcept override { return true; }

    void reset() noexcept
    {
        posix_op::reset();
        iovec_count = 0;
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

/** Write operation state with buffer descriptors. */
struct posix_write_op : posix_op
{
    static constexpr std::size_t max_buffers = 16;
    iovec iovecs[max_buffers];
    int iovec_count = 0;

    void reset() noexcept
    {
        posix_op::reset();
        iovec_count = 0;
    }

    void perform_io() noexcept override
    {
        ssize_t n = ::writev(fd, iovecs, iovec_count);
        if (n >= 0)
            complete(0, static_cast<std::size_t>(n));
        else
            complete(errno, 0);
    }
};

//------------------------------------------------------------------------------

/** Accept operation state. */
struct posix_accept_op : posix_op
{
    int accepted_fd = -1;
    io_object::io_object_impl* peer_impl = nullptr;
    io_object::io_object_impl** impl_out = nullptr;

    using create_peer_fn = io_object::io_object_impl* (*)(void*, int);
    create_peer_fn create_peer = nullptr;
    void* service_ptr = nullptr;

    void reset() noexcept
    {
        posix_op::reset();
        accepted_fd = -1;
        peer_impl = nullptr;
        impl_out = nullptr;
        // Don't reset create_peer and service_ptr - they're set once
    }

    void perform_io() noexcept override
    {
        sockaddr_in addr{};
        socklen_t addrlen = sizeof(addr);
        int new_fd = ::accept4(fd, reinterpret_cast<sockaddr*>(&addr),
                               &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (new_fd >= 0)
        {
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

        bool success = (error == 0 && !cancelled.load(std::memory_order_acquire));

        if (ec_out)
        {
            if (cancelled.load(std::memory_order_acquire))
                *ec_out = make_error_code(system::errc::operation_canceled);
            else if (error != 0)
                *ec_out = system::error_code(error, system::system_category());
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

        d.dispatch(h).resume();
    }
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
