//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include "src/detail/epoll/sockets.hpp"
#include "src/detail/endpoint_convert.hpp"
#include "src/detail/make_err.hpp"
#include "src/detail/dispatch_coro.hpp"

#include <boost/corosio/detail/except.hpp>
#include <boost/capy/buffers.hpp>

#include <utility>

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

// Register an op with the reactor, handling cached edge events.
// Called under the EAGAIN/EINPROGRESS path when speculative I/O failed.
void
epoll_socket_impl::
register_op(
    epoll_op& op,
    epoll_op*& desc_slot,
    bool& ready_flag) noexcept
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

void
epoll_op::canceller::
operator()() const noexcept
{
    op->cancel();
}

void
epoll_connect_op::
cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

void
epoll_read_op::
cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

void
epoll_write_op::
cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

void
epoll_op::
operator()()
{
    stop_cb.reset();

    socket_impl_->svc_.scheduler().reset_inline_budget();

    if (cancelled.load(std::memory_order_acquire))
        *ec_out = capy::error::canceled;
    else if (errn != 0)
        *ec_out = make_err(errn);
    else if (is_read_operation() && bytes_transferred == 0)
        *ec_out = capy::error::eof;
    else
        *ec_out = {};

    *bytes_out = bytes_transferred;

    // Move to stack before resuming coroutine. The coroutine might close
    // the socket, releasing the last wrapper ref. If impl_ptr were the
    // last ref and we destroyed it while still in operator(), we'd have
    // use-after-free. Moving to local ensures destruction happens at
    // function exit, after all member accesses are complete.
    capy::executor_ref saved_ex( std::move( ex ) );
    std::coroutine_handle<> saved_h( std::move( h ) );
    auto prevent_premature_destruction = std::move(impl_ptr);
    dispatch_coro(saved_ex, saved_h).resume();
}

void
epoll_connect_op::
operator()()
{
    stop_cb.reset();

    socket_impl_->svc_.scheduler().reset_inline_budget();

    bool success = (errn == 0 && !cancelled.load(std::memory_order_acquire));

    // Cache endpoints on successful connect
    if (success && socket_impl_)
    {
        // Query local endpoint via getsockname (may fail, but remote is always known)
        endpoint local_ep;
        sockaddr_in local_addr{};
        socklen_t local_len = sizeof(local_addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
            local_ep = from_sockaddr_in(local_addr);
        // Always cache remote endpoint; local may be default if getsockname failed
        static_cast<epoll_socket_impl*>(socket_impl_)->set_endpoints(local_ep, target_endpoint);
    }

    if (cancelled.load(std::memory_order_acquire))
        *ec_out = capy::error::canceled;
    else if (errn != 0)
        *ec_out = make_err(errn);
    else
        *ec_out = {};

    // Move to stack before resuming. See epoll_op::operator()() for rationale.
    capy::executor_ref saved_ex( std::move( ex ) );
    std::coroutine_handle<> saved_h( std::move( h ) );
    auto prevent_premature_destruction = std::move(impl_ptr);
    dispatch_coro(saved_ex, saved_h).resume();
}

epoll_socket_impl::
epoll_socket_impl(epoll_socket_service& svc) noexcept
    : svc_(svc)
{
}

epoll_socket_impl::
~epoll_socket_impl() = default;

void
epoll_socket_impl::
release()
{
    close_socket();
    svc_.destroy_impl(*this);
}

std::coroutine_handle<>
epoll_socket_impl::
connect(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    endpoint ep,
    std::stop_token token,
    std::error_code* ec)
{
    auto& op = conn_;

    sockaddr_in addr = detail::to_sockaddr_in(ep);
    int result = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    if (result == 0)
    {
        sockaddr_in local_addr{};
        socklen_t local_len = sizeof(local_addr);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
            local_endpoint_ = detail::from_sockaddr_in(local_addr);
        remote_endpoint_ = ep;
    }

    if (result == 0 || errno != EINPROGRESS)
    {
        int err = (result < 0) ? errno : 0;
        if (svc_.scheduler().try_consume_inline_budget())
        {
            *ec = err ? make_err(err) : std::error_code{};
            return ex.dispatch(h);
        }
        op.reset();
        op.h = h;
        op.ex = ex;
        op.ec_out = ec;
        op.fd = fd_;
        op.target_endpoint = ep;
        op.start(token, this);
        op.impl_ptr = shared_from_this();
        op.complete(err, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    // EINPROGRESS — register with reactor
    op.reset();
    op.h = h;
    op.ex = ex;
    op.ec_out = ec;
    op.fd = fd_;
    op.target_endpoint = ep;
    op.start(token, this);
    op.impl_ptr = shared_from_this();

    register_op(op, desc_state_.connect_op, desc_state_.write_ready);
    return std::noop_coroutine();
}

std::coroutine_handle<>
epoll_socket_impl::
read_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    io_buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = rd_;
    op.reset();

    capy::mutable_buffer bufs[epoll_read_op::max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, epoll_read_op::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.empty_buffer_read = true;
        op.h = h;
        op.ex = ex;
        op.ec_out = ec;
        op.bytes_out = bytes_out;
        op.start(token, this);
        op.impl_ptr = shared_from_this();
        op.complete(0, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len = bufs[i].size();
    }

    // Speculative read
    ssize_t n;
    do {
        n = ::readv(fd_, op.iovecs, op.iovec_count);
    } while (n < 0 && errno == EINTR);

    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
    {
        int err = (n < 0) ? errno : 0;
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
            return ex.dispatch(h);
        }
        op.h = h;
        op.ex = ex;
        op.ec_out = ec;
        op.bytes_out = bytes_out;
        op.start(token, this);
        op.impl_ptr = shared_from_this();
        op.complete(err, bytes);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    // EAGAIN — register with reactor
    op.h = h;
    op.ex = ex;
    op.ec_out = ec;
    op.bytes_out = bytes_out;
    op.fd = fd_;
    op.start(token, this);
    op.impl_ptr = shared_from_this();

    register_op(op, desc_state_.read_op, desc_state_.read_ready);
    return std::noop_coroutine();
}

std::coroutine_handle<>
epoll_socket_impl::
write_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    io_buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = wr_;
    op.reset();

    capy::mutable_buffer bufs[epoll_write_op::max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, epoll_write_op::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.h = h;
        op.ex = ex;
        op.ec_out = ec;
        op.bytes_out = bytes_out;
        op.start(token, this);
        op.impl_ptr = shared_from_this();
        op.complete(0, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len = bufs[i].size();
    }

    // Speculative write
    msghdr msg{};
    msg.msg_iov = op.iovecs;
    msg.msg_iovlen = static_cast<std::size_t>(op.iovec_count);

    ssize_t n;
    do {
        n = ::sendmsg(fd_, &msg, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);

    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
    {
        int err = (n < 0) ? errno : 0;
        auto bytes = (n > 0) ? static_cast<std::size_t>(n) : std::size_t(0);

        if (svc_.scheduler().try_consume_inline_budget())
        {
            *ec = err ? make_err(err) : std::error_code{};
            *bytes_out = bytes;
            return ex.dispatch(h);
        }
        op.h = h;
        op.ex = ex;
        op.ec_out = ec;
        op.bytes_out = bytes_out;
        op.start(token, this);
        op.impl_ptr = shared_from_this();
        op.complete(err, bytes);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    // EAGAIN — register with reactor
    op.h = h;
    op.ex = ex;
    op.ec_out = ec;
    op.bytes_out = bytes_out;
    op.fd = fd_;
    op.start(token, this);
    op.impl_ptr = shared_from_this();

    register_op(op, desc_state_.write_op, desc_state_.write_ready);
    return std::noop_coroutine();
}

std::error_code
epoll_socket_impl::
shutdown(tcp_socket::shutdown_type what) noexcept
{
    int how;
    switch (what)
    {
    case tcp_socket::shutdown_receive: how = SHUT_RD;   break;
    case tcp_socket::shutdown_send:    how = SHUT_WR;   break;
    case tcp_socket::shutdown_both:    how = SHUT_RDWR; break;
    default:
        return make_err(EINVAL);
    }
    if (::shutdown(fd_, how) != 0)
        return make_err(errno);
    return {};
}

std::error_code
epoll_socket_impl::
set_no_delay(bool value) noexcept
{
    int flag = value ? 1 : 0;
    if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) != 0)
        return make_err(errno);
    return {};
}

bool
epoll_socket_impl::
no_delay(std::error_code& ec) const noexcept
{
    int flag = 0;
    socklen_t len = sizeof(flag);
    if (::getsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, &len) != 0)
    {
        ec = make_err(errno);
        return false;
    }
    ec = {};
    return flag != 0;
}

std::error_code
epoll_socket_impl::
set_keep_alive(bool value) noexcept
{
    int flag = value ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag)) != 0)
        return make_err(errno);
    return {};
}

bool
epoll_socket_impl::
keep_alive(std::error_code& ec) const noexcept
{
    int flag = 0;
    socklen_t len = sizeof(flag);
    if (::getsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &flag, &len) != 0)
    {
        ec = make_err(errno);
        return false;
    }
    ec = {};
    return flag != 0;
}

std::error_code
epoll_socket_impl::
set_receive_buffer_size(int size) noexcept
{
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) != 0)
        return make_err(errno);
    return {};
}

int
epoll_socket_impl::
receive_buffer_size(std::error_code& ec) const noexcept
{
    int size = 0;
    socklen_t len = sizeof(size);
    if (::getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, &len) != 0)
    {
        ec = make_err(errno);
        return 0;
    }
    ec = {};
    return size;
}

std::error_code
epoll_socket_impl::
set_send_buffer_size(int size) noexcept
{
    if (::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) != 0)
        return make_err(errno);
    return {};
}

int
epoll_socket_impl::
send_buffer_size(std::error_code& ec) const noexcept
{
    int size = 0;
    socklen_t len = sizeof(size);
    if (::getsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, &len) != 0)
    {
        ec = make_err(errno);
        return 0;
    }
    ec = {};
    return size;
}

std::error_code
epoll_socket_impl::
set_linger(bool enabled, int timeout) noexcept
{
    if (timeout < 0)
        return make_err(EINVAL);
    struct ::linger lg;
    lg.l_onoff = enabled ? 1 : 0;
    lg.l_linger = timeout;
    if (::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) != 0)
        return make_err(errno);
    return {};
}

tcp_socket::linger_options
epoll_socket_impl::
linger(std::error_code& ec) const noexcept
{
    struct ::linger lg{};
    socklen_t len = sizeof(lg);
    if (::getsockopt(fd_, SOL_SOCKET, SO_LINGER, &lg, &len) != 0)
    {
        ec = make_err(errno);
        return {};
    }
    ec = {};
    return {.enabled = lg.l_onoff != 0, .timeout = lg.l_linger};
}

void
epoll_socket_impl::
cancel() noexcept
{
    std::shared_ptr<epoll_socket_impl> self;
    try {
        self = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return;
    }

    conn_.request_cancel();
    rd_.request_cancel();
    wr_.request_cancel();

    epoll_op* conn_claimed = nullptr;
    epoll_op* rd_claimed = nullptr;
    epoll_op* wr_claimed = nullptr;
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

void
epoll_socket_impl::
cancel_single_op(epoll_op& op) noexcept
{
    op.request_cancel();

    epoll_op** desc_op_ptr = nullptr;
    if (&op == &conn_) desc_op_ptr = &desc_state_.connect_op;
    else if (&op == &rd_) desc_op_ptr = &desc_state_.read_op;
    else if (&op == &wr_) desc_op_ptr = &desc_state_.write_op;

    if (desc_op_ptr)
    {
        epoll_op* claimed = nullptr;
        {
            std::lock_guard lock(desc_state_.mutex);
            if (*desc_op_ptr == &op)
                claimed = std::exchange(*desc_op_ptr, nullptr);
        }
        if (claimed)
        {
            try {
                op.impl_ptr = shared_from_this();
            } catch (const std::bad_weak_ptr&) {}
            svc_.post(&op);
            svc_.work_finished();
        }
    }
}

void
epoll_socket_impl::
close_socket() noexcept
{
    cancel();

    // Keep impl alive if descriptor_state is queued in the scheduler.
    // Without this, destroy_impl() drops the last shared_ptr while
    // the queued descriptor_state node would become dangling.
    if (desc_state_.is_enqueued_.load(std::memory_order_acquire))
    {
        try {
            desc_state_.impl_ref_ = shared_from_this();
        } catch (std::bad_weak_ptr const&) {}
    }

    if (fd_ >= 0)
    {
        if (desc_state_.registered_events != 0)
            svc_.scheduler().deregister_descriptor(fd_);
        ::close(fd_);
        fd_ = -1;
    }

    desc_state_.fd = -1;
    {
        std::lock_guard lock(desc_state_.mutex);
        desc_state_.read_op = nullptr;
        desc_state_.write_op = nullptr;
        desc_state_.connect_op = nullptr;
        desc_state_.read_ready = false;
        desc_state_.write_ready = false;
    }
    desc_state_.registered_events = 0;

    local_endpoint_ = endpoint{};
    remote_endpoint_ = endpoint{};
}

epoll_socket_service::
epoll_socket_service(capy::execution_context& ctx)
    : state_(std::make_unique<epoll_socket_state>(ctx.use_service<epoll_scheduler>()))
{
}

epoll_socket_service::
~epoll_socket_service()
{
}

void
epoll_socket_service::
shutdown()
{
    std::lock_guard lock(state_->mutex_);

    while (auto* impl = state_->socket_list_.pop_front())
        impl->close_socket();

    state_->socket_ptrs_.clear();
}

tcp_socket::socket_impl&
epoll_socket_service::
create_impl()
{
    auto impl = std::make_shared<epoll_socket_impl>(*this);
    auto* raw = impl.get();

    {
        std::lock_guard lock(state_->mutex_);
        state_->socket_list_.push_back(raw);
        state_->socket_ptrs_.emplace(raw, std::move(impl));
    }

    return *raw;
}

void
epoll_socket_service::
destroy_impl(tcp_socket::socket_impl& impl)
{
    auto* epoll_impl = static_cast<epoll_socket_impl*>(&impl);
    std::lock_guard lock(state_->mutex_);
    state_->socket_list_.remove(epoll_impl);
    state_->socket_ptrs_.erase(epoll_impl);
}

std::error_code
epoll_socket_service::
open_socket(tcp_socket::socket_impl& impl)
{
    auto* epoll_impl = static_cast<epoll_socket_impl*>(&impl);
    epoll_impl->close_socket();

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return make_err(errno);

    epoll_impl->fd_ = fd;

    // Register fd with epoll (edge-triggered mode)
    epoll_impl->desc_state_.fd = fd;
    {
        std::lock_guard lock(epoll_impl->desc_state_.mutex);
        epoll_impl->desc_state_.read_op = nullptr;
        epoll_impl->desc_state_.write_op = nullptr;
        epoll_impl->desc_state_.connect_op = nullptr;
    }
    scheduler().register_descriptor(fd, &epoll_impl->desc_state_);

    return {};
}

void
epoll_socket_service::
post(epoll_op* op)
{
    state_->sched_.post(op);
}

void
epoll_socket_service::
work_started() noexcept
{
    state_->sched_.work_started();
}

void
epoll_socket_service::
work_finished() noexcept
{
    state_->sched_.work_finished();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL
