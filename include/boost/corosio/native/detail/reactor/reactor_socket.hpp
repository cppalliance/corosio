//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SOCKET_HPP

#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op_base.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/capy/buffers.hpp>

#include <coroutine>
#include <memory>
#include <mutex>
#include <utility>

#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace boost::corosio::detail {

/** CRTP base for reactor-backed socket implementations.

    Provides shared data members, trivial virtual overrides,
    non-virtual helper methods for cancellation, registration,
    close, and the full I/O dispatch logic (`do_connect`,
    `do_read_some`, `do_write_some`). Concrete backends inherit
    and add `cancel()`, `close_socket()`, and I/O overrides that
    delegate to the `do_*` helpers.

    @tparam Derived   The concrete socket type (CRTP).
    @tparam Service   The backend's socket service type.
    @tparam Op        The backend's base op type.
    @tparam ConnOp    The backend's connect op type.
    @tparam ReadOp    The backend's read op type.
    @tparam WriteOp   The backend's write op type.
    @tparam DescState The backend's descriptor_state type.
*/
template<
    class Derived,
    class Service,
    class Op,
    class ConnOp,
    class ReadOp,
    class WriteOp,
    class DescState>
class reactor_socket
    : public tcp_socket::implementation
    , public std::enable_shared_from_this<Derived>
    , public intrusive_list<Derived>::node
{
    friend Derived;

    explicit reactor_socket(Service& svc) noexcept : svc_(svc) {}

protected:
    Service& svc_;
    int fd_ = -1;
    endpoint local_endpoint_;
    endpoint remote_endpoint_;

public:
    /// Pending connect operation slot.
    ConnOp conn_;

    /// Pending read operation slot.
    ReadOp rd_;

    /// Pending write operation slot.
    WriteOp wr_;

    /// Per-descriptor state for persistent reactor registration.
    DescState desc_state_;

    ~reactor_socket() override = default;

    /// Return the underlying file descriptor.
    native_handle_type native_handle() const noexcept override
    {
        return fd_;
    }

    /// Return the cached local endpoint.
    endpoint local_endpoint() const noexcept override
    {
        return local_endpoint_;
    }

    /// Return the cached remote endpoint.
    endpoint remote_endpoint() const noexcept override
    {
        return remote_endpoint_;
    }

    /// Return true if the socket has an open file descriptor.
    bool is_open() const noexcept
    {
        return fd_ >= 0;
    }

    /// Shut down part or all of the full-duplex connection.
    std::error_code shutdown(tcp_socket::shutdown_type what) noexcept override
    {
        int how;
        switch (what)
        {
        case tcp_socket::shutdown_receive:
            how = SHUT_RD;
            break;
        case tcp_socket::shutdown_send:
            how = SHUT_WR;
            break;
        case tcp_socket::shutdown_both:
            how = SHUT_RDWR;
            break;
        default:
            return make_err(EINVAL);
        }
        if (::shutdown(fd_, how) != 0)
            return make_err(errno);
        return {};
    }

    /// Set a socket option.
    std::error_code set_option(
        int level,
        int optname,
        void const* data,
        std::size_t size) noexcept override
    {
        if (::setsockopt(
                fd_, level, optname, data, static_cast<socklen_t>(size)) != 0)
            return make_err(errno);
        return {};
    }

    /// Get a socket option.
    std::error_code
    get_option(int level, int optname, void* data, std::size_t* size)
        const noexcept override
    {
        socklen_t len = static_cast<socklen_t>(*size);
        if (::getsockopt(fd_, level, optname, data, &len) != 0)
            return make_err(errno);
        *size = static_cast<std::size_t>(len);
        return {};
    }

    /// Assign the file descriptor.
    void set_socket(int fd) noexcept
    {
        fd_ = fd;
    }

    /// Cache local and remote endpoints.
    void set_endpoints(endpoint local, endpoint remote) noexcept
    {
        local_endpoint_  = local;
        remote_endpoint_ = remote;
    }

    /** Register an op with the reactor.

        Handles cached edge events and deferred cancellation.
        Called on the EAGAIN/EINPROGRESS path when speculative
        I/O failed.
    */
    void register_op(
        Op& op,
        reactor_op_base*& desc_slot,
        bool& ready_flag,
        bool& cancel_flag) noexcept;

    /** Cancel a single pending operation.

        Claims the operation from its descriptor_state slot under
        the mutex and posts it to the scheduler as cancelled.

        @param op The operation to cancel.
    */
    void cancel_single_op(Op& op) noexcept;

    /** Cancel all pending operations.

        Invoked by the derived class's cancel() override.
    */
    void do_cancel() noexcept;

    /** Close the socket and cancel pending operations.

        Invoked by the derived class's close_socket(). The
        derived class may add backend-specific cleanup after
        calling this method.
    */
    void do_close_socket() noexcept;

    /** Shared connect dispatch.

        Tries the connect syscall speculatively. On synchronous
        completion, returns via inline budget or posts through queue.
        On EINPROGRESS, registers with the reactor.
    */
    std::coroutine_handle<> do_connect(
        std::coroutine_handle<>,
        capy::executor_ref,
        endpoint,
        std::stop_token const&,
        std::error_code*);

    /** Shared scatter-read dispatch.

        Tries readv() speculatively. On success or hard error,
        returns via inline budget or posts through queue.
        On EAGAIN, registers with the reactor.
    */
    std::coroutine_handle<> do_read_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token const&,
        std::error_code*,
        std::size_t*);

    /** Shared gather-write dispatch.

        Tries the write via WriteOp::write_policy speculatively.
        On success or hard error, returns via inline budget or
        posts through queue. On EAGAIN, registers with the reactor.
    */
    std::coroutine_handle<> do_write_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token const&,
        std::error_code*,
        std::size_t*);
};

template<
    class Derived,
    class Service,
    class Op,
    class ConnOp,
    class ReadOp,
    class WriteOp,
    class DescState>
void
reactor_socket<Derived, Service, Op, ConnOp, ReadOp, WriteOp, DescState>::
    register_op(
        Op& op,
        reactor_op_base*& desc_slot,
        bool& ready_flag,
        bool& cancel_flag) noexcept
{
    svc_.work_started();

    std::lock_guard lock(desc_state_.mutex);
    bool io_done = false;
    if (ready_flag)
    {
        ready_flag = false;
        op.perform_io();
        io_done = (op.errn != EAGAIN && op.errn != EWOULDBLOCK);
        if (!io_done)
            op.errn = 0;
    }

    if (cancel_flag)
    {
        cancel_flag = false;
        op.cancelled.store(true, std::memory_order_relaxed);
    }

    if (io_done || op.cancelled.load(std::memory_order_acquire))
    {
        svc_.post(&op);
        svc_.work_finished();
    }
    else
    {
        desc_slot = &op;
    }
}

template<
    class Derived,
    class Service,
    class Op,
    class ConnOp,
    class ReadOp,
    class WriteOp,
    class DescState>
void
reactor_socket<Derived, Service, Op, ConnOp, ReadOp, WriteOp, DescState>::
    cancel_single_op(Op& op) noexcept
{
    auto self = this->weak_from_this().lock();
    if (!self)
        return;

    op.request_cancel();

    reactor_op_base** desc_op_ptr = nullptr;
    if (&op == &conn_)
        desc_op_ptr = &desc_state_.connect_op;
    else if (&op == &rd_)
        desc_op_ptr = &desc_state_.read_op;
    else if (&op == &wr_)
        desc_op_ptr = &desc_state_.write_op;

    if (desc_op_ptr)
    {
        reactor_op_base* claimed = nullptr;
        {
            std::lock_guard lock(desc_state_.mutex);
            if (*desc_op_ptr == &op)
                claimed = std::exchange(*desc_op_ptr, nullptr);
            else if (&op == &conn_)
                desc_state_.connect_cancel_pending = true;
            else if (&op == &rd_)
                desc_state_.read_cancel_pending = true;
            else if (&op == &wr_)
                desc_state_.write_cancel_pending = true;
        }
        if (claimed)
        {
            op.impl_ptr = self;
            svc_.post(&op);
            svc_.work_finished();
        }
    }
}

template<
    class Derived,
    class Service,
    class Op,
    class ConnOp,
    class ReadOp,
    class WriteOp,
    class DescState>
void
reactor_socket<Derived, Service, Op, ConnOp, ReadOp, WriteOp, DescState>::
    do_cancel() noexcept
{
    auto self = this->weak_from_this().lock();
    if (!self)
        return;

    conn_.request_cancel();
    rd_.request_cancel();
    wr_.request_cancel();

    reactor_op_base* conn_claimed = nullptr;
    reactor_op_base* rd_claimed   = nullptr;
    reactor_op_base* wr_claimed   = nullptr;
    {
        std::lock_guard lock(desc_state_.mutex);
        if (desc_state_.connect_op == &conn_)
            conn_claimed = std::exchange(desc_state_.connect_op, nullptr);
        if (desc_state_.read_op == &rd_)
            rd_claimed = std::exchange(desc_state_.read_op, nullptr);
        if (desc_state_.write_op == &wr_)
            wr_claimed = std::exchange(desc_state_.write_op, nullptr);
    }

    if (conn_claimed)
    {
        conn_.impl_ptr = self;
        svc_.post(&conn_);
        svc_.work_finished();
    }
    if (rd_claimed)
    {
        rd_.impl_ptr = self;
        svc_.post(&rd_);
        svc_.work_finished();
    }
    if (wr_claimed)
    {
        wr_.impl_ptr = self;
        svc_.post(&wr_);
        svc_.work_finished();
    }
}

template<
    class Derived,
    class Service,
    class Op,
    class ConnOp,
    class ReadOp,
    class WriteOp,
    class DescState>
void
reactor_socket<Derived, Service, Op, ConnOp, ReadOp, WriteOp, DescState>::
    do_close_socket() noexcept
{
    auto self = this->weak_from_this().lock();
    if (self)
    {
        conn_.request_cancel();
        rd_.request_cancel();
        wr_.request_cancel();

        reactor_op_base* conn_claimed = nullptr;
        reactor_op_base* rd_claimed   = nullptr;
        reactor_op_base* wr_claimed   = nullptr;
        {
            std::lock_guard lock(desc_state_.mutex);
            conn_claimed = std::exchange(desc_state_.connect_op, nullptr);
            rd_claimed   = std::exchange(desc_state_.read_op, nullptr);
            wr_claimed   = std::exchange(desc_state_.write_op, nullptr);
            desc_state_.read_ready             = false;
            desc_state_.write_ready            = false;
            desc_state_.read_cancel_pending    = false;
            desc_state_.write_cancel_pending   = false;
            desc_state_.connect_cancel_pending = false;

            // Keep impl alive while descriptor_state is queued in the
            // scheduler. Must be under mutex to avoid racing with
            // invoke_deferred_io()'s move of impl_ref_.
            if (desc_state_.is_enqueued_.load(std::memory_order_acquire))
                desc_state_.impl_ref_ = self;
        }

        if (conn_claimed)
        {
            conn_.impl_ptr = self;
            svc_.post(&conn_);
            svc_.work_finished();
        }
        if (rd_claimed)
        {
            rd_.impl_ptr = self;
            svc_.post(&rd_);
            svc_.work_finished();
        }
        if (wr_claimed)
        {
            wr_.impl_ptr = self;
            svc_.post(&wr_);
            svc_.work_finished();
        }
    }

    if (fd_ >= 0)
    {
        if (desc_state_.registered_events != 0)
            svc_.scheduler().deregister_descriptor(fd_);
        ::close(fd_);
        fd_ = -1;
    }

    desc_state_.fd                = -1;
    desc_state_.registered_events = 0;

    local_endpoint_  = endpoint{};
    remote_endpoint_ = endpoint{};
}

template<
    class Derived,
    class Service,
    class Op,
    class ConnOp,
    class ReadOp,
    class WriteOp,
    class DescState>
std::coroutine_handle<>
reactor_socket<Derived, Service, Op, ConnOp, ReadOp, WriteOp, DescState>::
    do_connect(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        endpoint ep,
        std::stop_token const& token,
        std::error_code* ec)
{
    auto& op = conn_;

    sockaddr_storage storage{};
    socklen_t addrlen = to_sockaddr(ep, socket_family(fd_), storage);
    int result = ::connect(fd_, reinterpret_cast<sockaddr*>(&storage), addrlen);

    if (result == 0)
    {
        sockaddr_storage local_storage{};
        socklen_t local_len = sizeof(local_storage);
        if (::getsockname(
                fd_, reinterpret_cast<sockaddr*>(&local_storage), &local_len) ==
            0)
            local_endpoint_ = from_sockaddr(local_storage);
        remote_endpoint_ = ep;
    }

    if (result == 0 || errno != EINPROGRESS)
    {
        int err = (result < 0) ? errno : 0;
        if (svc_.scheduler().try_consume_inline_budget())
        {
            *ec = err ? make_err(err) : std::error_code{};
            return dispatch_coro(ex, h);
        }
        op.reset();
        op.h               = h;
        op.ex              = ex;
        op.ec_out          = ec;
        op.fd              = fd_;
        op.target_endpoint = ep;
        op.start(token, static_cast<Derived*>(this));
        op.impl_ptr = this->shared_from_this();
        op.complete(err, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    // EINPROGRESS — register with reactor
    op.reset();
    op.h               = h;
    op.ex              = ex;
    op.ec_out          = ec;
    op.fd              = fd_;
    op.target_endpoint = ep;
    op.start(token, static_cast<Derived*>(this));
    op.impl_ptr = this->shared_from_this();

    register_op(
        op, desc_state_.connect_op, desc_state_.write_ready,
        desc_state_.connect_cancel_pending);
    return std::noop_coroutine();
}

template<
    class Derived,
    class Service,
    class Op,
    class ConnOp,
    class ReadOp,
    class WriteOp,
    class DescState>
std::coroutine_handle<>
reactor_socket<Derived, Service, Op, ConnOp, ReadOp, WriteOp, DescState>::
    do_read_some(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        std::stop_token const& token,
        std::error_code* ec,
        std::size_t* bytes_out)
{
    auto& op = rd_;
    op.reset();

    capy::mutable_buffer bufs[ReadOp::max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, ReadOp::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.empty_buffer_read = true;
        op.h                 = h;
        op.ex                = ex;
        op.ec_out            = ec;
        op.bytes_out         = bytes_out;
        op.start(token, static_cast<Derived*>(this));
        op.impl_ptr = this->shared_from_this();
        op.complete(0, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len  = bufs[i].size();
    }

    // Speculative read
    ssize_t n;
    do
    {
        n = ::readv(fd_, op.iovecs, op.iovec_count);
    }
    while (n < 0 && errno == EINTR);

    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
    {
        int err    = (n < 0) ? errno : 0;
        auto bytes = (n > 0) ? static_cast<std::size_t>(n) : std::size_t(0);

        if (svc_.scheduler().try_consume_inline_budget())
        {
            if (err)
                *ec = make_err(err);
            else if (n == 0)
                *ec = capy::error::eof;
            else
                *ec = {};
            *bytes_out = bytes;
            return dispatch_coro(ex, h);
        }
        op.h         = h;
        op.ex        = ex;
        op.ec_out    = ec;
        op.bytes_out = bytes_out;
        op.start(token, static_cast<Derived*>(this));
        op.impl_ptr = this->shared_from_this();
        op.complete(err, bytes);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    // EAGAIN — register with reactor
    op.h         = h;
    op.ex        = ex;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.fd        = fd_;
    op.start(token, static_cast<Derived*>(this));
    op.impl_ptr = this->shared_from_this();

    register_op(
        op, desc_state_.read_op, desc_state_.read_ready,
        desc_state_.read_cancel_pending);
    return std::noop_coroutine();
}

template<
    class Derived,
    class Service,
    class Op,
    class ConnOp,
    class ReadOp,
    class WriteOp,
    class DescState>
std::coroutine_handle<>
reactor_socket<Derived, Service, Op, ConnOp, ReadOp, WriteOp, DescState>::
    do_write_some(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        std::stop_token const& token,
        std::error_code* ec,
        std::size_t* bytes_out)
{
    auto& op = wr_;
    op.reset();

    capy::mutable_buffer bufs[WriteOp::max_buffers];
    op.iovec_count =
        static_cast<int>(param.copy_to(bufs, WriteOp::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.h         = h;
        op.ex        = ex;
        op.ec_out    = ec;
        op.bytes_out = bytes_out;
        op.start(token, static_cast<Derived*>(this));
        op.impl_ptr = this->shared_from_this();
        op.complete(0, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len  = bufs[i].size();
    }

    // Speculative write via backend-specific write policy
    ssize_t n = WriteOp::write_policy::write(fd_, op.iovecs, op.iovec_count);

    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
    {
        int err    = (n < 0) ? errno : 0;
        auto bytes = (n > 0) ? static_cast<std::size_t>(n) : std::size_t(0);

        if (svc_.scheduler().try_consume_inline_budget())
        {
            *ec        = err ? make_err(err) : std::error_code{};
            *bytes_out = bytes;
            return dispatch_coro(ex, h);
        }
        op.h         = h;
        op.ex        = ex;
        op.ec_out    = ec;
        op.bytes_out = bytes_out;
        op.start(token, static_cast<Derived*>(this));
        op.impl_ptr = this->shared_from_this();
        op.complete(err, bytes);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    // EAGAIN — register with reactor
    op.h         = h;
    op.ex        = ex;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.fd        = fd_;
    op.start(token, static_cast<Derived*>(this));
    op.impl_ptr = this->shared_from_this();

    register_op(
        op, desc_state_.write_op, desc_state_.write_ready,
        desc_state_.write_cancel_pending);
    return std::noop_coroutine();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SOCKET_HPP
