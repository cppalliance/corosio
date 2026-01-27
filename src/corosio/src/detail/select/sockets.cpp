//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//


#if !defined(_WIN32)

#include "src/detail/select/sockets.hpp"
#include "src/detail/endpoint_convert.hpp"
#include "src/detail/make_err.hpp"

#include <boost/capy/buffers.hpp>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

//------------------------------------------------------------------------------
// select_op::canceller - implements stop_token cancellation
//------------------------------------------------------------------------------

void
select_op::canceller::
operator()() const noexcept
{
    // When stop_token is signaled, we need to actually cancel the I/O operation,
    // not just set a flag. Otherwise the operation stays blocked on select.
    if (op->socket_impl_)
        op->socket_impl_->cancel_single_op(*op);
    else if (op->acceptor_impl_)
        op->acceptor_impl_->cancel_single_op(*op);
    else
        op->request_cancel();  // fallback: just set flag (legacy behavior)
}

//------------------------------------------------------------------------------
// select_connect_op::operator() - caches endpoints on successful connect
//------------------------------------------------------------------------------

void
select_connect_op::
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
        static_cast<select_socket_impl*>(socket_impl_)->set_endpoints(local_ep, target_endpoint);
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

    auto saved_d = d;
    auto saved_h = std::move(h);
    impl_ptr.reset();
    saved_d.dispatch(saved_h).resume();
}

//------------------------------------------------------------------------------
// select_accept_op::operator() - creates peer socket and caches endpoints
//------------------------------------------------------------------------------

void
select_accept_op::
operator()()
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

    if (success && accepted_fd >= 0)
    {
        if (acceptor_impl_)
        {
            auto* socket_svc = static_cast<select_acceptor_impl*>(acceptor_impl_)
                ->service().socket_service();
            if (socket_svc)
            {
                auto& impl = static_cast<select_socket_impl&>(socket_svc->create_impl());
                impl.set_socket(accepted_fd);

                sockaddr_in local_addr{};
                socklen_t local_len = sizeof(local_addr);
                sockaddr_in remote_addr{};
                socklen_t remote_len = sizeof(remote_addr);

                endpoint local_ep, remote_ep;
                if (::getsockname(accepted_fd, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
                    local_ep = from_sockaddr_in(local_addr);
                if (::getpeername(accepted_fd, reinterpret_cast<sockaddr*>(&remote_addr), &remote_len) == 0)
                    remote_ep = from_sockaddr_in(remote_addr);

                impl.set_endpoints(local_ep, remote_ep);

                if (impl_out)
                    *impl_out = &impl;

                accepted_fd = -1;
            }
            else
            {
                if (ec_out && !*ec_out)
                    *ec_out = make_err(ENOENT);
                ::close(accepted_fd);
                accepted_fd = -1;
                if (impl_out)
                    *impl_out = nullptr;
            }
        }
        else
        {
            ::close(accepted_fd);
            accepted_fd = -1;
            if (impl_out)
                *impl_out = nullptr;
        }
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

//------------------------------------------------------------------------------
// select_socket_impl
//------------------------------------------------------------------------------

select_socket_impl::
select_socket_impl(select_socket_service& svc) noexcept
    : svc_(svc)
{
}

void
select_socket_impl::
release()
{
    close_socket();
    svc_.destroy_impl(*this);
}

void
select_socket_impl::
connect(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    endpoint ep,
    std::stop_token token,
    system::error_code* ec)
{
    auto& op = conn_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.fd = fd_;
    op.target_endpoint = ep;  // Store target for endpoint caching
    op.start(token, this);

    sockaddr_in addr = detail::to_sockaddr_in(ep);
    int result = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    if (result == 0)
    {
        // Sync success - cache endpoints immediately
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
        op.registered.store(select_registration_state::registering, std::memory_order_release);
        svc_.scheduler().register_fd(fd_, &op, select_scheduler::event_write);

        // Transition to registered. If this fails, reactor or cancel already
        // claimed the op (state is now unregistered), so we're done. However,
        // we must still deregister the fd because cancel's deregister_fd may
        // have run before our register_fd, leaving the fd orphaned.
        auto expected = select_registration_state::registering;
        if (!op.registered.compare_exchange_strong(
                expected, select_registration_state::registered, std::memory_order_acq_rel))
        {
            svc_.scheduler().deregister_fd(fd_, select_scheduler::event_write);
            return;
        }

        // If cancelled was set before we registered, handle it now.
        if (op.cancelled.load(std::memory_order_acquire))
        {
            auto prev = op.registered.exchange(
                select_registration_state::unregistered, std::memory_order_acq_rel);
            if (prev != select_registration_state::unregistered)
            {
                svc_.scheduler().deregister_fd(fd_, select_scheduler::event_write);
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
select_socket_impl::
read_some(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    io_buffer_param param,
    std::stop_token token,
    system::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = rd_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.bytes_out = bytes_out;
    op.fd = fd_;
    op.start(token, this);

    capy::mutable_buffer bufs[select_read_op::max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, select_read_op::max_buffers));

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
        op.registered.store(select_registration_state::registering, std::memory_order_release);
        svc_.scheduler().register_fd(fd_, &op, select_scheduler::event_read);

        // Transition to registered. If this fails, reactor or cancel already
        // claimed the op (state is now unregistered), so we're done. However,
        // we must still deregister the fd because cancel's deregister_fd may
        // have run before our register_fd, leaving the fd orphaned.
        auto expected = select_registration_state::registering;
        if (!op.registered.compare_exchange_strong(
                expected, select_registration_state::registered, std::memory_order_acq_rel))
        {
            svc_.scheduler().deregister_fd(fd_, select_scheduler::event_read);
            return;
        }

        // If cancelled was set before we registered, handle it now.
        if (op.cancelled.load(std::memory_order_acquire))
        {
            auto prev = op.registered.exchange(
                select_registration_state::unregistered, std::memory_order_acq_rel);
            if (prev != select_registration_state::unregistered)
            {
                svc_.scheduler().deregister_fd(fd_, select_scheduler::event_read);
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
select_socket_impl::
write_some(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    io_buffer_param param,
    std::stop_token token,
    system::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = wr_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.bytes_out = bytes_out;
    op.fd = fd_;
    op.start(token, this);

    capy::mutable_buffer bufs[select_write_op::max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, select_write_op::max_buffers));

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
        op.registered.store(select_registration_state::registering, std::memory_order_release);
        svc_.scheduler().register_fd(fd_, &op, select_scheduler::event_write);

        // Transition to registered. If this fails, reactor or cancel already
        // claimed the op (state is now unregistered), so we're done. However,
        // we must still deregister the fd because cancel's deregister_fd may
        // have run before our register_fd, leaving the fd orphaned.
        auto expected = select_registration_state::registering;
        if (!op.registered.compare_exchange_strong(
                expected, select_registration_state::registered, std::memory_order_acq_rel))
        {
            svc_.scheduler().deregister_fd(fd_, select_scheduler::event_write);
            return;
        }

        // If cancelled was set before we registered, handle it now.
        if (op.cancelled.load(std::memory_order_acquire))
        {
            auto prev = op.registered.exchange(
                select_registration_state::unregistered, std::memory_order_acq_rel);
            if (prev != select_registration_state::unregistered)
            {
                svc_.scheduler().deregister_fd(fd_, select_scheduler::event_write);
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
select_socket_impl::
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
select_socket_impl::
set_no_delay(bool value) noexcept
{
    int flag = value ? 1 : 0;
    if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) != 0)
        return make_err(errno);
    return {};
}

bool
select_socket_impl::
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
select_socket_impl::
set_keep_alive(bool value) noexcept
{
    int flag = value ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag)) != 0)
        return make_err(errno);
    return {};
}

bool
select_socket_impl::
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
select_socket_impl::
set_receive_buffer_size(int size) noexcept
{
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) != 0)
        return make_err(errno);
    return {};
}

int
select_socket_impl::
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
select_socket_impl::
set_send_buffer_size(int size) noexcept
{
    if (::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) != 0)
        return make_err(errno);
    return {};
}

int
select_socket_impl::
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
select_socket_impl::
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
select_socket_impl::
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
select_socket_impl::
cancel() noexcept
{
    std::shared_ptr<select_socket_impl> self;
    try {
        self = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return;
    }

    auto cancel_op = [this, &self](select_op& op, int events) {
        auto prev = op.registered.exchange(
            select_registration_state::unregistered, std::memory_order_acq_rel);
        op.request_cancel();
        if (prev != select_registration_state::unregistered)
        {
            svc_.scheduler().deregister_fd(fd_, events);
            op.impl_ptr = self;
            svc_.post(&op);
            svc_.work_finished();
        }
    };

    cancel_op(conn_, select_scheduler::event_write);
    cancel_op(rd_, select_scheduler::event_read);
    cancel_op(wr_, select_scheduler::event_write);
}

void
select_socket_impl::
cancel_single_op(select_op& op) noexcept
{
    // Called from stop_token callback to cancel a specific pending operation.
    auto prev = op.registered.exchange(
        select_registration_state::unregistered, std::memory_order_acq_rel);
    op.request_cancel();

    if (prev != select_registration_state::unregistered)
    {
        // Determine which event type to deregister
        int events = 0;
        if (&op == &conn_ || &op == &wr_)
            events = select_scheduler::event_write;
        else if (&op == &rd_)
            events = select_scheduler::event_read;

        svc_.scheduler().deregister_fd(fd_, events);

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
select_socket_impl::
close_socket() noexcept
{
    cancel();

    if (fd_ >= 0)
    {
        // Unconditionally remove from registered_fds_ to handle edge cases
        // where the fd might be registered but cancel() didn't clean it up
        // due to race conditions.
        svc_.scheduler().deregister_fd(fd_,
            select_scheduler::event_read | select_scheduler::event_write);
        ::close(fd_);
        fd_ = -1;
    }

    // Clear cached endpoints
    local_endpoint_ = endpoint{};
    remote_endpoint_ = endpoint{};
}

//------------------------------------------------------------------------------
// select_acceptor_impl
//------------------------------------------------------------------------------

select_acceptor_impl::
select_acceptor_impl(select_acceptor_service& svc) noexcept
    : svc_(svc)
{
}

void
select_acceptor_impl::
release()
{
    close_socket();
    svc_.destroy_acceptor_impl(*this);
}

void
select_acceptor_impl::
accept(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    std::stop_token token,
    system::error_code* ec,
    io_object::io_object_impl** impl_out)
{
    auto& op = acc_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.impl_out = impl_out;
    op.fd = fd_;
    op.start(token, this);

    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    int accepted = ::accept(fd_, reinterpret_cast<sockaddr*>(&addr), &addrlen);

    if (accepted >= 0)
    {
        // Reject fds that exceed select()'s FD_SETSIZE limit.
        // Better to fail now than during later async operations.
        if (accepted >= FD_SETSIZE)
        {
            ::close(accepted);
            op.accepted_fd = -1;
            op.complete(EINVAL, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return;
        }

        // Set non-blocking and close-on-exec flags.
        // A non-blocking socket is essential for the async reactor;
        // if we can't configure it, fail rather than risk blocking.
        int flags = ::fcntl(accepted, F_GETFL, 0);
        if (flags == -1)
        {
            int err = errno;
            ::close(accepted);
            op.accepted_fd = -1;
            op.complete(err, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return;
        }

        if (::fcntl(accepted, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            int err = errno;
            ::close(accepted);
            op.accepted_fd = -1;
            op.complete(err, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return;
        }

        if (::fcntl(accepted, F_SETFD, FD_CLOEXEC) == -1)
        {
            int err = errno;
            ::close(accepted);
            op.accepted_fd = -1;
            op.complete(err, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return;
        }

        op.accepted_fd = accepted;
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
        op.registered.store(select_registration_state::registering, std::memory_order_release);
        svc_.scheduler().register_fd(fd_, &op, select_scheduler::event_read);

        // Transition to registered. If this fails, reactor or cancel already
        // claimed the op (state is now unregistered), so we're done. However,
        // we must still deregister the fd because cancel's deregister_fd may
        // have run before our register_fd, leaving the fd orphaned.
        auto expected = select_registration_state::registering;
        if (!op.registered.compare_exchange_strong(
                expected, select_registration_state::registered, std::memory_order_acq_rel))
        {
            svc_.scheduler().deregister_fd(fd_, select_scheduler::event_read);
            return;
        }

        // If cancelled was set before we registered, handle it now.
        if (op.cancelled.load(std::memory_order_acquire))
        {
            auto prev = op.registered.exchange(
                select_registration_state::unregistered, std::memory_order_acq_rel);
            if (prev != select_registration_state::unregistered)
            {
                svc_.scheduler().deregister_fd(fd_, select_scheduler::event_read);
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
select_acceptor_impl::
cancel() noexcept
{
    std::shared_ptr<select_acceptor_impl> self;
    try {
        self = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return;
    }

    auto prev = acc_.registered.exchange(
        select_registration_state::unregistered, std::memory_order_acq_rel);
    acc_.request_cancel();

    if (prev != select_registration_state::unregistered)
    {
        svc_.scheduler().deregister_fd(fd_, select_scheduler::event_read);
        acc_.impl_ptr = self;
        svc_.post(&acc_);
        svc_.work_finished();
    }
}

void
select_acceptor_impl::
cancel_single_op(select_op& op) noexcept
{
    // Called from stop_token callback to cancel a specific pending operation.
    auto prev = op.registered.exchange(
        select_registration_state::unregistered, std::memory_order_acq_rel);
    op.request_cancel();

    if (prev != select_registration_state::unregistered)
    {
        svc_.scheduler().deregister_fd(fd_, select_scheduler::event_read);

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
select_acceptor_impl::
close_socket() noexcept
{
    cancel();

    if (fd_ >= 0)
    {
        // Unconditionally remove from registered_fds_ to handle edge cases
        svc_.scheduler().deregister_fd(fd_, select_scheduler::event_read);
        ::close(fd_);
        fd_ = -1;
    }

    // Clear cached endpoint
    local_endpoint_ = endpoint{};
}

//------------------------------------------------------------------------------
// select_socket_service
//------------------------------------------------------------------------------

select_socket_service::
select_socket_service(capy::execution_context& ctx)
    : state_(std::make_unique<select_socket_state>(ctx.use_service<select_scheduler>()))
{
}

select_socket_service::
~select_socket_service()
{
}

void
select_socket_service::
shutdown()
{
    std::lock_guard lock(state_->mutex_);

    while (auto* impl = state_->socket_list_.pop_front())
        impl->close_socket();

    state_->socket_ptrs_.clear();
}

socket::socket_impl&
select_socket_service::
create_impl()
{
    auto impl = std::make_shared<select_socket_impl>(*this);
    auto* raw = impl.get();

    {
        std::lock_guard lock(state_->mutex_);
        state_->socket_list_.push_back(raw);
        state_->socket_ptrs_.emplace(raw, std::move(impl));
    }

    return *raw;
}

void
select_socket_service::
destroy_impl(socket::socket_impl& impl)
{
    auto* select_impl = static_cast<select_socket_impl*>(&impl);
    std::lock_guard lock(state_->mutex_);
    state_->socket_list_.remove(select_impl);
    state_->socket_ptrs_.erase(select_impl);
}

system::error_code
select_socket_service::
open_socket(socket::socket_impl& impl)
{
    auto* select_impl = static_cast<select_socket_impl*>(&impl);
    select_impl->close_socket();

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return make_err(errno);

    // Set non-blocking and close-on-exec
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }

    // Check fd is within select() limits
    if (fd >= FD_SETSIZE)
    {
        ::close(fd);
        return make_err(EMFILE);  // Too many open files
    }

    select_impl->fd_ = fd;
    return {};
}

void
select_socket_service::
post(select_op* op)
{
    state_->sched_.post(op);
}

void
select_socket_service::
work_started() noexcept
{
    state_->sched_.work_started();
}

void
select_socket_service::
work_finished() noexcept
{
    state_->sched_.work_finished();
}

//------------------------------------------------------------------------------
// select_acceptor_service
//------------------------------------------------------------------------------

select_acceptor_service::
select_acceptor_service(capy::execution_context& ctx)
    : ctx_(ctx)
    , state_(std::make_unique<select_acceptor_state>(ctx.use_service<select_scheduler>()))
{
}

select_acceptor_service::
~select_acceptor_service()
{
}

void
select_acceptor_service::
shutdown()
{
    std::lock_guard lock(state_->mutex_);

    while (auto* impl = state_->acceptor_list_.pop_front())
        impl->close_socket();

    state_->acceptor_ptrs_.clear();
}

acceptor::acceptor_impl&
select_acceptor_service::
create_acceptor_impl()
{
    auto impl = std::make_shared<select_acceptor_impl>(*this);
    auto* raw = impl.get();

    std::lock_guard lock(state_->mutex_);
    state_->acceptor_list_.push_back(raw);
    state_->acceptor_ptrs_.emplace(raw, std::move(impl));

    return *raw;
}

void
select_acceptor_service::
destroy_acceptor_impl(acceptor::acceptor_impl& impl)
{
    auto* select_impl = static_cast<select_acceptor_impl*>(&impl);
    std::lock_guard lock(state_->mutex_);
    state_->acceptor_list_.remove(select_impl);
    state_->acceptor_ptrs_.erase(select_impl);
}

system::error_code
select_acceptor_service::
open_acceptor(
    acceptor::acceptor_impl& impl,
    endpoint ep,
    int backlog)
{
    auto* select_impl = static_cast<select_acceptor_impl*>(&impl);
    select_impl->close_socket();

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return make_err(errno);

    // Set non-blocking and close-on-exec
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }

    // Check fd is within select() limits
    if (fd >= FD_SETSIZE)
    {
        ::close(fd);
        return make_err(EMFILE);
    }

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr = detail::to_sockaddr_in(ep);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }

    if (::listen(fd, backlog) < 0)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }

    select_impl->fd_ = fd;

    // Cache the local endpoint (queries OS for ephemeral port if port was 0)
    sockaddr_in local_addr{};
    socklen_t local_len = sizeof(local_addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
        select_impl->set_local_endpoint(detail::from_sockaddr_in(local_addr));

    return {};
}

void
select_acceptor_service::
post(select_op* op)
{
    state_->sched_.post(op);
}

void
select_acceptor_service::
work_started() noexcept
{
    state_->sched_.work_started();
}

void
select_acceptor_service::
work_finished() noexcept
{
    state_->sched_.work_finished();
}

select_socket_service*
select_acceptor_service::
socket_service() const noexcept
{
    auto* svc = ctx_.find_service<detail::socket_service>();
    return svc ? dynamic_cast<select_socket_service*>(svc) : nullptr;
}

} // namespace boost::corosio::detail

#endif // !defined(_WIN32)
