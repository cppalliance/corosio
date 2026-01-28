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

#include <boost/corosio/detail/except.hpp>
#include <boost/capy/buffers.hpp>

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

//------------------------------------------------------------------------------
// epoll_op::canceller - implements stop_token cancellation
//------------------------------------------------------------------------------

void
epoll_op::canceller::
operator()() const noexcept
{
    op->cancel();
}

//------------------------------------------------------------------------------
// cancel() overrides for socket operations
//------------------------------------------------------------------------------

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

//------------------------------------------------------------------------------
// epoll_connect_op::operator() - caches endpoints on successful connect
//------------------------------------------------------------------------------

void
epoll_connect_op::
operator()()
{
    stop_cb.reset();

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

    if (ec_out)
    {
        if (cancelled.load(std::memory_order_acquire))
            *ec_out = capy::error::canceled;
        else if (errn != 0)
            *ec_out = make_err(errn);
    }

    if (bytes_out)
        *bytes_out = bytes_transferred;

    // Move to stack before destroying the frame
    capy::executor_ref saved_ex( std::move( ex ) );
    capy::coro saved_h( std::move( h ) );
    impl_ptr.reset();
    saved_ex.dispatch( saved_h ).resume();
}

//------------------------------------------------------------------------------
// epoll_socket_impl
//------------------------------------------------------------------------------

epoll_socket_impl::
epoll_socket_impl(epoll_socket_service& svc) noexcept
    : svc_(svc)
{
}

void
epoll_socket_impl::
release()
{
    close_socket();
    svc_.destroy_impl(*this);
}

void
epoll_socket_impl::
connect(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    endpoint ep,
    std::stop_token token,
    system::error_code* ec)
{
    auto& op = conn_;
    op.reset();
    op.h = h;
    op.ex = ex;
    op.ec_out = ec;
    op.fd = fd_;
    op.target_endpoint = ep;  // Store target for endpoint caching
    op.start(token, this);

    sockaddr_in addr = detail::to_sockaddr_in(ep);
    int result = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    if (result == 0)
    {
        // Sync success - cache endpoints immediately
        // Remote is always known; local may fail but we still cache remote
        sockaddr_in local_addr{};
        socklen_t local_len = sizeof(local_addr);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
            local_endpoint_ = detail::from_sockaddr_in(local_addr);
        remote_endpoint_ = ep;

        op.complete(0, 0);
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return;
    }

    if (errno == EINPROGRESS)
    {
        svc_.work_started();
        // Set registering BEFORE register_fd to close the race window where
        // reactor sees an event before we set registered. The reactor treats
        // registering the same as registered when claiming the op.
        op.registered.store(registration_state::registering, std::memory_order_release);
        svc_.scheduler().register_fd(fd_, &op, EPOLLOUT | EPOLLET);

        // Transition to registered. If this fails, reactor or cancel already
        // claimed the op (state is now unregistered), so we're done. However,
        // we must still unregister the fd because cancel's unregister_fd may
        // have run before our register_fd, leaving the fd orphaned in epoll.
        auto expected = registration_state::registering;
        if (!op.registered.compare_exchange_strong(
                expected, registration_state::registered, std::memory_order_acq_rel))
        {
            svc_.scheduler().unregister_fd(fd_);
            return;
        }

        // If cancelled was set before we registered, handle it now.
        if (op.cancelled.load(std::memory_order_acquire))
        {
            auto prev = op.registered.exchange(
                registration_state::unregistered, std::memory_order_acq_rel);
            if (prev != registration_state::unregistered)
            {
                svc_.scheduler().unregister_fd(fd_);
                op.impl_ptr = shared_from_this();
                svc_.post(&op);
                svc_.work_finished();
            }
        }
        return;
    }

    op.complete(errno, 0);
    op.impl_ptr = shared_from_this();
    svc_.post(&op);
}

void
epoll_socket_impl::
read_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    io_buffer_param param,
    std::stop_token token,
    system::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = rd_;
    op.reset();
    op.h = h;
    op.ex = ex;
    op.ec_out = ec;
    op.bytes_out = bytes_out;
    op.fd = fd_;
    op.start(token, this);

    capy::mutable_buffer bufs[epoll_read_op::max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, epoll_read_op::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.empty_buffer_read = true;
        op.complete(0, 0);
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return;
    }

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len = bufs[i].size();
    }

    ssize_t n = ::readv(fd_, op.iovecs, op.iovec_count);

    if (n > 0)
    {
        op.complete(0, static_cast<std::size_t>(n));
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return;
    }

    if (n == 0)
    {
        op.complete(0, 0);
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        svc_.work_started();
        // Set registering BEFORE register_fd to close the race window where
        // reactor sees an event before we set registered.
        op.registered.store(registration_state::registering, std::memory_order_release);
        svc_.scheduler().register_fd(fd_, &op, EPOLLIN | EPOLLET);

        // Transition to registered. If this fails, reactor or cancel already
        // claimed the op (state is now unregistered), so we're done. However,
        // we must still unregister the fd because cancel's unregister_fd may
        // have run before our register_fd, leaving the fd orphaned in epoll.
        auto expected = registration_state::registering;
        if (!op.registered.compare_exchange_strong(
                expected, registration_state::registered, std::memory_order_acq_rel))
        {
            svc_.scheduler().unregister_fd(fd_);
            return;
        }

        // If cancelled was set before we registered, handle it now.
        if (op.cancelled.load(std::memory_order_acquire))
        {
            auto prev = op.registered.exchange(
                registration_state::unregistered, std::memory_order_acq_rel);
            if (prev != registration_state::unregistered)
            {
                svc_.scheduler().unregister_fd(fd_);
                op.impl_ptr = shared_from_this();
                svc_.post(&op);
                svc_.work_finished();
            }
        }
        return;
    }

    op.complete(errno, 0);
    op.impl_ptr = shared_from_this();
    svc_.post(&op);
}

void
epoll_socket_impl::
write_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    io_buffer_param param,
    std::stop_token token,
    system::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = wr_;
    op.reset();
    op.h = h;
    op.ex = ex;
    op.ec_out = ec;
    op.bytes_out = bytes_out;
    op.fd = fd_;
    op.start(token, this);

    capy::mutable_buffer bufs[epoll_write_op::max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, epoll_write_op::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.complete(0, 0);
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return;
    }

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len = bufs[i].size();
    }

    msghdr msg{};
    msg.msg_iov = op.iovecs;
    msg.msg_iovlen = static_cast<std::size_t>(op.iovec_count);

    ssize_t n = ::sendmsg(fd_, &msg, MSG_NOSIGNAL);

    if (n > 0)
    {
        op.complete(0, static_cast<std::size_t>(n));
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        svc_.work_started();
        // Set registering BEFORE register_fd to close the race window where
        // reactor sees an event before we set registered.
        op.registered.store(registration_state::registering, std::memory_order_release);
        svc_.scheduler().register_fd(fd_, &op, EPOLLOUT | EPOLLET);

        // Transition to registered. If this fails, reactor or cancel already
        // claimed the op (state is now unregistered), so we're done. However,
        // we must still unregister the fd because cancel's unregister_fd may
        // have run before our register_fd, leaving the fd orphaned in epoll.
        auto expected = registration_state::registering;
        if (!op.registered.compare_exchange_strong(
                expected, registration_state::registered, std::memory_order_acq_rel))
        {
            svc_.scheduler().unregister_fd(fd_);
            return;
        }

        // If cancelled was set before we registered, handle it now.
        if (op.cancelled.load(std::memory_order_acquire))
        {
            auto prev = op.registered.exchange(
                registration_state::unregistered, std::memory_order_acq_rel);
            if (prev != registration_state::unregistered)
            {
                svc_.scheduler().unregister_fd(fd_);
                op.impl_ptr = shared_from_this();
                svc_.post(&op);
                svc_.work_finished();
            }
        }
        return;
    }

    op.complete(errno ? errno : EIO, 0);
    op.impl_ptr = shared_from_this();
    svc_.post(&op);
}

system::error_code
epoll_socket_impl::
shutdown(socket::shutdown_type what) noexcept
{
    int how;
    switch (what)
    {
    case socket::shutdown_receive: how = SHUT_RD;   break;
    case socket::shutdown_send:    how = SHUT_WR;   break;
    case socket::shutdown_both:    how = SHUT_RDWR; break;
    default:
        return make_err(EINVAL);
    }
    if (::shutdown(fd_, how) != 0)
        return make_err(errno);
    return {};
}

//------------------------------------------------------------------------------
// Socket Options
//------------------------------------------------------------------------------

system::error_code
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
no_delay(system::error_code& ec) const noexcept
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

system::error_code
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
keep_alive(system::error_code& ec) const noexcept
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

system::error_code
epoll_socket_impl::
set_receive_buffer_size(int size) noexcept
{
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) != 0)
        return make_err(errno);
    return {};
}

int
epoll_socket_impl::
receive_buffer_size(system::error_code& ec) const noexcept
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

system::error_code
epoll_socket_impl::
set_send_buffer_size(int size) noexcept
{
    if (::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) != 0)
        return make_err(errno);
    return {};
}

int
epoll_socket_impl::
send_buffer_size(system::error_code& ec) const noexcept
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

system::error_code
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

socket::linger_options
epoll_socket_impl::
linger(system::error_code& ec) const noexcept
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

    auto cancel_op = [this, &self](epoll_op& op) {
        auto prev = op.registered.exchange(
            registration_state::unregistered, std::memory_order_acq_rel);
        op.request_cancel();
        if (prev != registration_state::unregistered)
        {
            svc_.scheduler().unregister_fd(fd_);
            op.impl_ptr = self;
            svc_.post(&op);
            svc_.work_finished();
        }
    };

    cancel_op(conn_);
    cancel_op(rd_);
    cancel_op(wr_);
}

void
epoll_socket_impl::
cancel_single_op(epoll_op& op) noexcept
{
    // Called from stop_token callback to cancel a specific pending operation.
    // This performs actual I/O cancellation, not just setting a flag.
    auto prev = op.registered.exchange(
        registration_state::unregistered, std::memory_order_acq_rel);
    op.request_cancel();

    if (prev != registration_state::unregistered)
    {
        svc_.scheduler().unregister_fd(fd_);

        // Keep impl alive until op completes
        try {
            op.impl_ptr = shared_from_this();
        } catch (const std::bad_weak_ptr&) {
            // Impl is being destroyed, op will be orphaned but that's ok
        }

        svc_.post(&op);
        svc_.work_finished();
    }
}

void
epoll_socket_impl::
close_socket() noexcept
{
    cancel();

    if (fd_ >= 0)
    {
        // Unconditionally remove from epoll to handle edge cases where
        // the fd might be registered but cancel() didn't clean it up
        // due to race conditions. Note: kernel auto-removes on close,
        // but this is defensive and makes behavior consistent with select.
        svc_.scheduler().unregister_fd(fd_);
        ::close(fd_);
        fd_ = -1;
    }

    // Clear cached endpoints
    local_endpoint_ = endpoint{};
    remote_endpoint_ = endpoint{};
}

//------------------------------------------------------------------------------
// epoll_socket_service
//------------------------------------------------------------------------------

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

socket::socket_impl&
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
destroy_impl(socket::socket_impl& impl)
{
    auto* epoll_impl = static_cast<epoll_socket_impl*>(&impl);
    std::lock_guard lock(state_->mutex_);
    state_->socket_list_.remove(epoll_impl);
    state_->socket_ptrs_.erase(epoll_impl);
}

system::error_code
epoll_socket_service::
open_socket(socket::socket_impl& impl)
{
    auto* epoll_impl = static_cast<epoll_socket_impl*>(&impl);
    epoll_impl->close_socket();

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return make_err(errno);

    epoll_impl->fd_ = fd;
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
