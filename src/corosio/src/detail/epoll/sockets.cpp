//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_EPOLL)

#include "src/detail/epoll/sockets.hpp"
#include "src/detail/endpoint_convert.hpp"
#include "src/detail/make_err.hpp"

#include <boost/capy/buffers.hpp>

#include <errno.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost {
namespace corosio {
namespace detail {

//------------------------------------------------------------------------------
// epoll_socket_impl
//------------------------------------------------------------------------------

epoll_socket_impl::
epoll_socket_impl(epoll_sockets& svc) noexcept
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
    op.start(token);

    sockaddr_in addr = detail::to_sockaddr_in(ep);
    int result = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    if (result == 0)
    {
        op.complete(0, 0);
        svc_.post(&op);
        return;
    }

    if (errno == EINPROGRESS)
    {
        svc_.work_started();
        op.registered.store(true, std::memory_order_release);
        svc_.scheduler().register_fd(fd_, &op, EPOLLOUT | EPOLLET);
        return;
    }

    op.complete(errno, 0);
    svc_.post(&op);
}

void
epoll_socket_impl::
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
    op.start(token);

    capy::mutable_buffer bufs[epoll_read_op::max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, epoll_read_op::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.empty_buffer_read = true;
        op.complete(0, 0);
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
        svc_.post(&op);
        return;
    }

    if (n == 0)
    {
        op.complete(0, 0);
        svc_.post(&op);
        return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        svc_.work_started();
        op.registered.store(true, std::memory_order_release);
        svc_.scheduler().register_fd(fd_, &op, EPOLLIN | EPOLLET);
        return;
    }

    op.complete(errno, 0);
    svc_.post(&op);
}

void
epoll_socket_impl::
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
    op.start(token);

    capy::mutable_buffer bufs[epoll_write_op::max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, epoll_write_op::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.complete(0, 0);
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
        svc_.post(&op);
        return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        svc_.work_started();
        op.registered.store(true, std::memory_order_release);
        svc_.scheduler().register_fd(fd_, &op, EPOLLOUT | EPOLLET);
        return;
    }

    op.complete(errno ? errno : EIO, 0);
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
        bool was_registered = op.registered.exchange(false, std::memory_order_acq_rel);
        op.request_cancel();
        if (was_registered)
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
close_socket() noexcept
{
    cancel();

    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

//------------------------------------------------------------------------------
// epoll_acceptor_impl
//------------------------------------------------------------------------------

epoll_acceptor_impl::
epoll_acceptor_impl(epoll_sockets& svc) noexcept
    : svc_(svc)
{
}

void
epoll_acceptor_impl::
release()
{
    close_socket();
    svc_.destroy_acceptor_impl(*this);
}

void
epoll_acceptor_impl::
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
    op.start(token);

    op.service_ptr = &svc_;
    op.create_peer = [](void* svc_ptr, int new_fd) -> io_object::io_object_impl* {
        auto& svc = *static_cast<epoll_sockets*>(svc_ptr);
        auto& peer_impl = svc.create_impl();
        peer_impl.set_socket(new_fd);
        return &peer_impl;
    };

    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    int accepted = ::accept4(fd_, reinterpret_cast<sockaddr*>(&addr),
                             &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (accepted >= 0)
    {
        auto& peer_impl = svc_.create_impl();
        peer_impl.set_socket(accepted);
        op.accepted_fd = accepted;
        op.peer_impl = &peer_impl;
        op.complete(0, 0);
        svc_.post(&op);
        return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        svc_.work_started();
        op.registered.store(true, std::memory_order_release);
        svc_.scheduler().register_fd(fd_, &op, EPOLLIN | EPOLLET);
        return;
    }

    op.complete(errno, 0);
    svc_.post(&op);
}

void
epoll_acceptor_impl::
cancel() noexcept
{
    std::shared_ptr<epoll_acceptor_impl> self;
    try {
        self = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return;
    }

    bool was_registered = acc_.registered.exchange(false, std::memory_order_acq_rel);
    acc_.request_cancel();

    if (was_registered)
    {
        svc_.scheduler().unregister_fd(fd_);
        acc_.impl_ptr = self;
        svc_.post(&acc_);
        svc_.work_finished();
    }
}

void
epoll_acceptor_impl::
close_socket() noexcept
{
    cancel();

    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

//------------------------------------------------------------------------------
// epoll_sockets
//------------------------------------------------------------------------------

epoll_sockets::
epoll_sockets(capy::execution_context& ctx)
    : sched_(ctx.use_service<epoll_scheduler>())
{
}

epoll_sockets::
~epoll_sockets()
{
}

void
epoll_sockets::
shutdown()
{
    std::lock_guard lock(mutex_);

    while (auto* impl = socket_list_.pop_front())
        impl->close_socket();

    while (auto* impl = acceptor_list_.pop_front())
        impl->close_socket();

    socket_ptrs_.clear();
    acceptor_ptrs_.clear();
}

epoll_socket_impl&
epoll_sockets::
create_impl()
{
    auto impl = std::make_shared<epoll_socket_impl>(*this);
    auto* raw = impl.get();

    {
        std::lock_guard lock(mutex_);
        socket_list_.push_back(raw);
        socket_ptrs_.emplace(raw, std::move(impl));
    }

    return *raw;
}

void
epoll_sockets::
destroy_impl(epoll_socket_impl& impl)
{
    std::lock_guard lock(mutex_);
    socket_list_.remove(&impl);
    socket_ptrs_.erase(&impl);
}

system::error_code
epoll_sockets::
open_socket(epoll_socket_impl& impl)
{
    impl.close_socket();

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return make_err(errno);

    impl.fd_ = fd;
    return {};
}

epoll_acceptor_impl&
epoll_sockets::
create_acceptor_impl()
{
    auto impl = std::make_shared<epoll_acceptor_impl>(*this);
    auto* raw = impl.get();

    {
        std::lock_guard lock(mutex_);
        acceptor_list_.push_back(raw);
        acceptor_ptrs_.emplace(raw, std::move(impl));
    }

    return *raw;
}

void
epoll_sockets::
destroy_acceptor_impl(epoll_acceptor_impl& impl)
{
    std::lock_guard lock(mutex_);
    acceptor_list_.remove(&impl);
    acceptor_ptrs_.erase(&impl);
}

system::error_code
epoll_sockets::
open_acceptor(
    epoll_acceptor_impl& impl,
    endpoint ep,
    int backlog)
{
    impl.close_socket();

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return make_err(errno);

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

    impl.fd_ = fd;
    return {};
}

void
epoll_sockets::
post(epoll_op* op)
{
    sched_.post(op);
}

void
epoll_sockets::
work_started() noexcept
{
    sched_.work_started();
}

void
epoll_sockets::
work_finished() noexcept
{
    sched_.work_finished();
}

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
