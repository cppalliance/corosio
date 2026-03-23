//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_OP_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_OP_HPP

#include <boost/corosio/native/detail/reactor/reactor_op_base.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/detail/continuation_op.hpp>
#include <boost/capy/ex/executor_ref.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <system_error>

#include <errno.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

namespace boost::corosio::detail {

/** Base operation for reactor-based backends.

    Holds per-operation state that depends on the concrete backend
    socket/acceptor types: coroutine handle, executor, output
    pointers, file descriptor, stop_callback, and type-specific
    impl pointers.

    Fields shared across all backends (errn, bytes_transferred,
    cancelled, impl_ptr, perform_io, complete) live in
    reactor_op_base so the scheduler and descriptor_state can
    access them without template instantiation.

    @tparam Socket The backend socket impl type (forward-declared).
    @tparam Acceptor The backend acceptor impl type (forward-declared).
*/
template<class Socket, class Acceptor>
struct reactor_op : reactor_op_base
{
    /// Stop-token callback that invokes cancel() on the target op.
    struct canceller
    {
        reactor_op* op;
        void operator()() const noexcept
        {
            op->cancel();
        }
    };

    /// Caller's coroutine handle to resume on completion.
    std::coroutine_handle<> h;

    /// Scheduler-ready continuation for executor dispatch/post (wraps h).
    detail::continuation_op cont_op;

    /// Executor for dispatching the completion.
    capy::executor_ref ex;

    /// Output pointer for the error code.
    std::error_code* ec_out = nullptr;

    /// Output pointer for bytes transferred.
    std::size_t* bytes_out = nullptr;

    /// File descriptor this operation targets.
    int fd = -1;

    /// Stop-token callback registration.
    std::optional<std::stop_callback<canceller>> stop_cb;

    /// Owning socket impl (for stop_token cancellation).
    Socket* socket_impl_ = nullptr;

    /// Owning acceptor impl (for stop_token cancellation).
    Acceptor* acceptor_impl_ = nullptr;

    reactor_op() = default;

    /// Reset operation state for reuse.
    void reset() noexcept
    {
        fd                = -1;
        errn              = 0;
        bytes_transferred = 0;
        cancelled.store(false, std::memory_order_relaxed);
        impl_ptr.reset();
        socket_impl_   = nullptr;
        acceptor_impl_ = nullptr;
    }

    /// Return true if this is a read-direction operation.
    virtual bool is_read_operation() const noexcept
    {
        return false;
    }

    /// Cancel this operation via the owning impl.
    virtual void cancel() noexcept = 0;

    /// Destroy without invoking.
    void destroy() override
    {
        stop_cb.reset();
        reactor_op_base::destroy();
    }

    /// Arm the stop-token callback for a socket operation.
    void start(std::stop_token const& token, Socket* impl)
    {
        cancelled.store(false, std::memory_order_release);
        stop_cb.reset();
        socket_impl_   = impl;
        acceptor_impl_ = nullptr;

        if (token.stop_possible())
            stop_cb.emplace(token, canceller{this});
    }

    /// Arm the stop-token callback for an acceptor operation.
    void start(std::stop_token const& token, Acceptor* impl)
    {
        cancelled.store(false, std::memory_order_release);
        stop_cb.reset();
        socket_impl_   = nullptr;
        acceptor_impl_ = impl;

        if (token.stop_possible())
            stop_cb.emplace(token, canceller{this});
    }
};

/** Shared connect operation.

    Checks SO_ERROR for connect completion status. The operator()()
    and cancel() are provided by the concrete backend type.

    @tparam Base The backend's base op type.
*/
template<class Base>
struct reactor_connect_op : Base
{
    /// Endpoint to connect to.
    endpoint target_endpoint;

    /// Reset operation state for reuse.
    void reset() noexcept
    {
        Base::reset();
        target_endpoint = endpoint{};
    }

    void perform_io() noexcept override
    {
        int err       = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(this->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
            err = errno;
        this->complete(err, 0);
    }
};

/** Shared scatter-read operation.

    Uses readv() with an EINTR retry loop.

    @tparam Base The backend's base op type.
*/
template<class Base>
struct reactor_read_op : Base
{
    /// Maximum scatter-gather buffer count.
    static constexpr std::size_t max_buffers = 16;

    /// Scatter-gather I/O vectors.
    iovec iovecs[max_buffers];

    /// Number of active I/O vectors.
    int iovec_count = 0;

    /// True for zero-length reads (completed immediately).
    bool empty_buffer_read = false;

    /// Return true (this is a read-direction operation).
    bool is_read_operation() const noexcept override
    {
        return !empty_buffer_read;
    }

    void reset() noexcept
    {
        Base::reset();
        iovec_count       = 0;
        empty_buffer_read = false;
    }

    void perform_io() noexcept override
    {
        ssize_t n;
        do
        {
            n = ::readv(this->fd, iovecs, iovec_count);
        }
        while (n < 0 && errno == EINTR);

        if (n >= 0)
            this->complete(0, static_cast<std::size_t>(n));
        else
            this->complete(errno, 0);
    }
};

/** Shared gather-write operation.

    Delegates the actual syscall to WritePolicy::write(fd, iovecs, count),
    which returns ssize_t (bytes written or -1 with errno set).

    @tparam Base The backend's base op type.
    @tparam WritePolicy Provides `static ssize_t write(int, iovec*, int)`.
*/
template<class Base, class WritePolicy>
struct reactor_write_op : Base
{
    /// The write syscall policy type.
    using write_policy = WritePolicy;

    /// Maximum scatter-gather buffer count.
    static constexpr std::size_t max_buffers = 16;

    /// Scatter-gather I/O vectors.
    iovec iovecs[max_buffers];

    /// Number of active I/O vectors.
    int iovec_count = 0;

    void reset() noexcept
    {
        Base::reset();
        iovec_count = 0;
    }

    void perform_io() noexcept override
    {
        ssize_t n = WritePolicy::write(this->fd, iovecs, iovec_count);
        if (n >= 0)
            this->complete(0, static_cast<std::size_t>(n));
        else
            this->complete(errno, 0);
    }
};

/** Shared accept operation.

    Delegates the actual syscall to AcceptPolicy::do_accept(fd, peer_storage),
    which returns the accepted fd or -1 with errno set.

    @tparam Base The backend's base op type.
    @tparam AcceptPolicy Provides `static int do_accept(int, sockaddr_storage&)`.
*/
template<class Base, class AcceptPolicy>
struct reactor_accept_op : Base
{
    /// File descriptor of the accepted connection.
    int accepted_fd = -1;

    /// Pointer to the peer socket implementation.
    io_object::implementation* peer_impl = nullptr;

    /// Output pointer for the accepted implementation.
    io_object::implementation** impl_out = nullptr;

    /// Peer address storage filled by accept.
    sockaddr_storage peer_storage{};

    void reset() noexcept
    {
        Base::reset();
        accepted_fd  = -1;
        peer_impl    = nullptr;
        impl_out     = nullptr;
        peer_storage = {};
    }

    void perform_io() noexcept override
    {
        int new_fd = AcceptPolicy::do_accept(this->fd, peer_storage);
        if (new_fd >= 0)
        {
            accepted_fd = new_fd;
            this->complete(0, 0);
        }
        else
        {
            this->complete(errno, 0);
        }
    }
};

/** Shared connected send operation for datagram sockets.

    Uses sendmsg() with msg_name=nullptr (connected mode).

    @tparam Base The backend's base op type.
*/
template<class Base>
struct reactor_send_op : Base
{
    /// Maximum scatter-gather buffer count.
    static constexpr std::size_t max_buffers = 16;

    /// Scatter-gather I/O vectors.
    iovec iovecs[max_buffers];

    /// Number of active I/O vectors.
    int iovec_count = 0;

    void reset() noexcept
    {
        Base::reset();
        iovec_count = 0;
    }

    void perform_io() noexcept override
    {
        msghdr msg{};
        msg.msg_iov    = iovecs;
        msg.msg_iovlen = static_cast<std::size_t>(iovec_count);

#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif

        ssize_t n;
        do
        {
            n = ::sendmsg(this->fd, &msg, send_flags);
        }
        while (n < 0 && errno == EINTR);

        if (n >= 0)
            this->complete(0, static_cast<std::size_t>(n));
        else
            this->complete(errno, 0);
    }
};

/** Shared connected recv operation for datagram sockets.

    Uses recvmsg() with msg_name=nullptr (connected mode).
    Unlike reactor_read_op, does not map n==0 to EOF
    (zero-length datagrams are valid).

    @tparam Base The backend's base op type.
*/
template<class Base>
struct reactor_recv_op : Base
{
    /// Maximum scatter-gather buffer count.
    static constexpr std::size_t max_buffers = 16;

    /// Scatter-gather I/O vectors.
    iovec iovecs[max_buffers];

    /// Number of active I/O vectors.
    int iovec_count = 0;

    /// Return true (this is a read-direction operation).
    bool is_read_operation() const noexcept override
    {
        return true;
    }

    void reset() noexcept
    {
        Base::reset();
        iovec_count = 0;
    }

    void perform_io() noexcept override
    {
        msghdr msg{};
        msg.msg_iov    = iovecs;
        msg.msg_iovlen = static_cast<std::size_t>(iovec_count);

        ssize_t n;
        do
        {
            n = ::recvmsg(this->fd, &msg, 0);
        }
        while (n < 0 && errno == EINTR);

        if (n >= 0)
            this->complete(0, static_cast<std::size_t>(n));
        else
            this->complete(errno, 0);
    }
};

/** Shared send_to operation for datagram sockets.

    Uses sendmsg() with the destination endpoint in msg_name.

    @tparam Base The backend's base op type.
*/
template<class Base>
struct reactor_send_to_op : Base
{
    /// Maximum scatter-gather buffer count.
    static constexpr std::size_t max_buffers = 16;

    /// Scatter-gather I/O vectors.
    iovec iovecs[max_buffers];

    /// Number of active I/O vectors.
    int iovec_count = 0;

    /// Destination address storage.
    sockaddr_storage dest_storage{};

    /// Destination address length.
    socklen_t dest_len = 0;

    void reset() noexcept
    {
        Base::reset();
        iovec_count  = 0;
        dest_storage = {};
        dest_len     = 0;
    }

    void perform_io() noexcept override
    {
        msghdr msg{};
        msg.msg_name    = &dest_storage;
        msg.msg_namelen = dest_len;
        msg.msg_iov     = iovecs;
        msg.msg_iovlen  = static_cast<std::size_t>(iovec_count);

#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif

        ssize_t n;
        do
        {
            n = ::sendmsg(this->fd, &msg, send_flags);
        }
        while (n < 0 && errno == EINTR);

        if (n >= 0)
            this->complete(0, static_cast<std::size_t>(n));
        else
            this->complete(errno, 0);
    }
};

/** Shared recv_from operation for datagram sockets.

    Uses recvmsg() with msg_name to capture the source endpoint.

    @tparam Base The backend's base op type.
*/
template<class Base>
struct reactor_recv_from_op : Base
{
    /// Maximum scatter-gather buffer count.
    static constexpr std::size_t max_buffers = 16;

    /// Scatter-gather I/O vectors.
    iovec iovecs[max_buffers];

    /// Number of active I/O vectors.
    int iovec_count = 0;

    /// Source address storage filled by recvmsg.
    sockaddr_storage source_storage{};

    /// Output pointer for the source endpoint (set by do_recv_from).
    endpoint* source_out = nullptr;

    /// Return true (this is a read-direction operation).
    bool is_read_operation() const noexcept override
    {
        return true;
    }

    void reset() noexcept
    {
        Base::reset();
        iovec_count    = 0;
        source_storage = {};
        source_out     = nullptr;
    }

    void perform_io() noexcept override
    {
        msghdr msg{};
        msg.msg_name    = &source_storage;
        msg.msg_namelen = sizeof(source_storage);
        msg.msg_iov     = iovecs;
        msg.msg_iovlen  = static_cast<std::size_t>(iovec_count);

        ssize_t n;
        do
        {
            n = ::recvmsg(this->fd, &msg, 0);
        }
        while (n < 0 && errno == EINTR);

        if (n >= 0)
            this->complete(0, static_cast<std::size_t>(n));
        else
            this->complete(errno, 0);
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_OP_HPP
