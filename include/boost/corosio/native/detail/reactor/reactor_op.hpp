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
    // The op envelope — coroutine handle h, cont_op, executor ex, ec_out,
    // bytes_out, cancelled, stop_cb (+ its canceller), impl_ptr — lives in
    // coro_op (via reactor_op_base) and is shared with io_uring/IOCP.
    // reactor_op adds only the reactor-specific routing state below.

    /// File descriptor this operation targets.
    int fd = -1;

    /// Owning socket impl (for stop_token cancellation routing).
    Socket* socket_impl_ = nullptr;

    /// Owning acceptor impl (for stop_token cancellation routing).
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

    /// coro_op cancellation hook (fired by the shared canceller when the
    /// stop_token requests cancellation): route to the impl-specific cancel().
    void on_cancel() noexcept override
    {
        cancel();
    }

    /// Destroy without invoking.
    void destroy() override
    {
        stop_cb.reset();
        reactor_op_base::destroy();
    }

    /// Arm the stop-token callback for a socket operation.
    void start(std::stop_token const& token, Socket* impl)
    {
        socket_impl_   = impl;
        acceptor_impl_ = nullptr;
        coro_op::start(token);
    }

    /// Arm the stop-token callback for an acceptor operation.
    void start(std::stop_token const& token, Acceptor* impl)
    {
        socket_impl_   = nullptr;
        acceptor_impl_ = impl;
        coro_op::start(token);
    }
};

/** Shared connect operation.

    Checks SO_ERROR for connect completion status. The operator()()
    and cancel() are provided by the concrete backend type.

    @tparam Base The backend's base op type.
    @tparam Endpoint The endpoint type (endpoint or local_endpoint).
*/
template<class Base, class Endpoint = endpoint>
struct reactor_connect_op : Base
{
    /// Endpoint to connect to.
    Endpoint target_endpoint;

    /// Reset operation state for reuse.
    void reset() noexcept
    {
        Base::reset();
        target_endpoint = Endpoint{};
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

/** Readiness-only wait operation.

    Does not perform any I/O syscall. Completion is signalled by
    the reactor delivering the requested edge event; reactor_descriptor_state
    calls complete() directly and never invokes perform_io().

    @tparam Base The backend's base op type.
*/
template<class Base>
struct reactor_wait_op : Base
{
    /* Mirror of reactor_event_read from reactor_descriptor_state.hpp.
       Including that header from here would create an include cycle
       (descriptor_state -> reactor_op_base; reactor_op -> reactor_op_base),
       so we carry the value locally. Both must stay in sync. */
    static constexpr std::uint32_t read_event = 0x001;

    /// Which event bit this wait targets (reactor_event_read/write/error).
    std::uint32_t wait_event = 0;

    void reset() noexcept
    {
        Base::reset();
        wait_event = 0;
    }

    bool is_read_operation() const noexcept override
    {
        return wait_event == read_event;
    }

    /* perform_io() should never be called for a wait op — readiness
       IS the completion. Overridden here to satisfy the virtual and
       produce a safe result if called defensively. */
    void perform_io() noexcept override
    {
        this->complete(0, 0);
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

    /// Peer address length returned by accept.
    socklen_t peer_addrlen = 0;

    void reset() noexcept
    {
        Base::reset();
        accepted_fd   = -1;
        peer_impl     = nullptr;
        impl_out      = nullptr;
        peer_storage  = {};
        peer_addrlen  = 0;
    }

    void perform_io() noexcept override
    {
        int new_fd = AcceptPolicy::do_accept(
            this->fd, peer_storage, peer_addrlen);
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

    /// User-supplied message flags.
    int msg_flags = 0;

    void reset() noexcept
    {
        Base::reset();
        iovec_count = 0;
        msg_flags   = 0;
    }

    void perform_io() noexcept override
    {
        msghdr msg{};
        msg.msg_iov    = iovecs;
        msg.msg_iovlen = static_cast<std::size_t>(iovec_count);

#ifdef MSG_NOSIGNAL
        int send_flags = msg_flags | MSG_NOSIGNAL;
#else
        int send_flags = msg_flags;
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

    /// User-supplied message flags.
    int msg_flags = 0;

    /// Return true (this is a read-direction operation).
    bool is_read_operation() const noexcept override
    {
        return true;
    }

    void reset() noexcept
    {
        Base::reset();
        iovec_count = 0;
        msg_flags   = 0;
    }

    void perform_io() noexcept override
    {
        msghdr msg{};
        msg.msg_iov    = iovecs;
        msg.msg_iovlen = static_cast<std::size_t>(iovec_count);

        ssize_t n;
        do
        {
            n = ::recvmsg(this->fd, &msg, msg_flags);
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

    /// User-supplied message flags.
    int msg_flags = 0;

    void reset() noexcept
    {
        Base::reset();
        iovec_count  = 0;
        dest_storage = {};
        dest_len     = 0;
        msg_flags    = 0;
    }

    void perform_io() noexcept override
    {
        msghdr msg{};
        msg.msg_name    = &dest_storage;
        msg.msg_namelen = dest_len;
        msg.msg_iov     = iovecs;
        msg.msg_iovlen  = static_cast<std::size_t>(iovec_count);

#ifdef MSG_NOSIGNAL
        int send_flags = msg_flags | MSG_NOSIGNAL;
#else
        int send_flags = msg_flags;
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
    @tparam Endpoint The endpoint type (endpoint or local_endpoint).
*/
template<class Base, class Endpoint = endpoint>
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

    /// Actual source address length returned by recvmsg.
    socklen_t source_addrlen = 0;

    /// Output pointer for the source endpoint (set by do_recv_from).
    Endpoint* source_out = nullptr;

    /// User-supplied message flags.
    int msg_flags = 0;

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
        source_addrlen = 0;
        source_out     = nullptr;
        msg_flags      = 0;
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
            n = ::recvmsg(this->fd, &msg, msg_flags);
        }
        while (n < 0 && errno == EINTR);

        if (n >= 0)
        {
            source_addrlen = msg.msg_namelen;
            this->complete(0, static_cast<std::size_t>(n));
        }
        else
            this->complete(errno, 0);
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_OP_HPP
