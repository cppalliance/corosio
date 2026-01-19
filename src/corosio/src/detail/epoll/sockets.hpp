//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_EPOLL_SOCKETS_HPP
#define BOOST_COROSIO_DETAIL_EPOLL_SOCKETS_HPP

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_EPOLL)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/acceptor.hpp>
#include <boost/corosio/socket.hpp>
#include <boost/capy/ex/any_executor_ref.hpp>
#include <boost/capy/concept/io_awaitable.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/core/intrusive_list.hpp>

#include "src/detail/epoll/op.hpp"
#include "src/detail/epoll/scheduler.hpp"
#include "src/detail/endpoint_convert.hpp"

#include <mutex>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost {
namespace corosio {
namespace detail {

class epoll_sockets;
class epoll_socket_impl;
class epoll_acceptor_impl;

//------------------------------------------------------------------------------

/** Socket implementation for epoll-based I/O.

    This class contains the state for a single socket, including
    the native socket handle and pending operations.
*/
class epoll_socket_impl
    : public socket::socket_impl
    , public capy::intrusive_list<epoll_socket_impl>::node
{
    friend class epoll_sockets;

public:
    explicit epoll_socket_impl(epoll_sockets& svc) noexcept;

    void release() override;

    void connect(
        std::coroutine_handle<>,
        capy::any_executor_ref,
        endpoint,
        std::stop_token,
        system::error_code*) override;

    void read_some(
        std::coroutine_handle<>,
        capy::any_executor_ref,
        capy::any_bufref&,
        std::stop_token,
        system::error_code*,
        std::size_t*) override;

    void write_some(
        std::coroutine_handle<>,
        capy::any_executor_ref,
        capy::any_bufref&,
        std::stop_token,
        system::error_code*,
        std::size_t*) override;

    int native_handle() const noexcept { return fd_; }
    bool is_open() const noexcept { return fd_ >= 0; }
    void cancel() noexcept;
    void close_socket() noexcept;
    void set_socket(int fd) noexcept { fd_ = fd; }

    epoll_connect_op conn_;
    epoll_read_op rd_;
    epoll_write_op wr_;

private:
    epoll_sockets& svc_;
    int fd_ = -1;
};

//------------------------------------------------------------------------------

/** Acceptor implementation for epoll-based I/O.

    This class contains the state for a listening socket.
*/
class epoll_acceptor_impl
    : public acceptor::acceptor_impl
    , public capy::intrusive_list<epoll_acceptor_impl>::node
{
    friend class epoll_sockets;

public:
    explicit epoll_acceptor_impl(epoll_sockets& svc) noexcept;

    void release() override;

    void accept(
        std::coroutine_handle<>,
        capy::any_executor_ref,
        std::stop_token,
        system::error_code*,
        io_object::io_object_impl**) override;

    int native_handle() const noexcept { return fd_; }
    bool is_open() const noexcept { return fd_ >= 0; }
    void cancel() noexcept;
    void close_socket() noexcept;

    epoll_accept_op acc_;

private:
    epoll_sockets& svc_;
    int fd_ = -1;
};

//------------------------------------------------------------------------------

/** epoll socket management service.

    This service owns all socket implementations and coordinates their
    lifecycle with the epoll-based scheduler.
*/
class epoll_sockets
    : public capy::execution_context::service
{
public:
    using key_type = epoll_sockets;

    /** Construct the socket service.

        @param ctx Reference to the owning execution_context.
    */
    explicit epoll_sockets(capy::execution_context& ctx);

    /** Destroy the socket service. */
    ~epoll_sockets();

    epoll_sockets(epoll_sockets const&) = delete;
    epoll_sockets& operator=(epoll_sockets const&) = delete;

    /** Shut down the service. */
    void shutdown() override;

    /** Create a new socket implementation. */
    epoll_socket_impl& create_impl();

    /** Destroy a socket implementation. */
    void destroy_impl(epoll_socket_impl& impl);

    /** Create and configure a socket.

        @param impl The socket implementation to initialize.
        @return Error code, or success.
    */
    system::error_code open_socket(epoll_socket_impl& impl);

    /** Create a new acceptor implementation. */
    epoll_acceptor_impl& create_acceptor_impl();

    /** Destroy an acceptor implementation. */
    void destroy_acceptor_impl(epoll_acceptor_impl& impl);

    /** Create, bind, and listen on an acceptor socket.

        @param impl The acceptor implementation to initialize.
        @param ep The local endpoint to bind to.
        @param backlog The listen backlog.
        @return Error code, or success.
    */
    system::error_code open_acceptor(
        epoll_acceptor_impl& impl,
        endpoint ep,
        int backlog);

    /** Return the scheduler. */
    epoll_scheduler& scheduler() const noexcept { return sched_; }

    /** Post an operation for completion. */
    void post(epoll_op* op);

    /** Notify scheduler of pending I/O work. */
    void work_started() noexcept;

    /** Notify scheduler that I/O work completed. */
    void work_finished() noexcept;

private:
    epoll_scheduler& sched_;
    std::mutex mutex_;
    capy::intrusive_list<epoll_socket_impl> socket_list_;
    capy::intrusive_list<epoll_acceptor_impl> acceptor_list_;
};

//------------------------------------------------------------------------------
// epoll_socket_impl implementation
//------------------------------------------------------------------------------

inline
epoll_socket_impl::
epoll_socket_impl(epoll_sockets& svc) noexcept
    : svc_(svc)
{
}

inline void
epoll_socket_impl::
release()
{
    close_socket();
    svc_.destroy_impl(*this);
}

inline void
epoll_socket_impl::
connect(
    std::coroutine_handle<> h,
    capy::any_executor_ref d,
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
        svc_.scheduler().register_fd(fd_, &op, EPOLLOUT | EPOLLET);
        return;
    }

    op.complete(errno, 0);
    svc_.post(&op);
}

inline void
epoll_socket_impl::
read_some(
    std::coroutine_handle<> h,
    capy::any_executor_ref d,
    capy::any_bufref& param,
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
        svc_.scheduler().register_fd(fd_, &op, EPOLLIN | EPOLLET);
        return;
    }

    op.complete(errno, 0);
    svc_.post(&op);
}

inline void
epoll_socket_impl::
write_some(
    std::coroutine_handle<> h,
    capy::any_executor_ref d,
    capy::any_bufref& param,
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
    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len = bufs[i].size();
    }

    ssize_t n = ::writev(fd_, op.iovecs, op.iovec_count);

    if (n > 0)
    {
        op.complete(0, static_cast<std::size_t>(n));
        svc_.post(&op);
        return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        svc_.work_started();
        svc_.scheduler().register_fd(fd_, &op, EPOLLOUT | EPOLLET);
        return;
    }

    // n == 0 shouldn't happen for TCP stream sockets
    op.complete(errno ? errno : EIO, 0);
    svc_.post(&op);
}

inline void
epoll_socket_impl::
cancel() noexcept
{
    conn_.request_cancel();
    rd_.request_cancel();
    wr_.request_cancel();
}

inline void
epoll_socket_impl::
close_socket() noexcept
{
    if (fd_ >= 0)
    {
        svc_.scheduler().unregister_fd(fd_);
        ::close(fd_);
        fd_ = -1;
    }
}

//------------------------------------------------------------------------------
// epoll_acceptor_impl implementation
//------------------------------------------------------------------------------

inline
epoll_acceptor_impl::
epoll_acceptor_impl(epoll_sockets& svc) noexcept
    : svc_(svc)
{
}

inline void
epoll_acceptor_impl::
release()
{
    close_socket();
    svc_.destroy_acceptor_impl(*this);
}

inline void
epoll_acceptor_impl::
accept(
    std::coroutine_handle<> h,
    capy::any_executor_ref d,
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

    // Callback for creating peer socket when accept completes via epoll
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
        svc_.scheduler().register_fd(fd_, &op, EPOLLIN | EPOLLET);
        return;
    }

    op.complete(errno, 0);
    svc_.post(&op);
}

inline void
epoll_acceptor_impl::
cancel() noexcept
{
    acc_.request_cancel();
}

inline void
epoll_acceptor_impl::
close_socket() noexcept
{
    if (fd_ >= 0)
    {
        svc_.scheduler().unregister_fd(fd_);
        ::close(fd_);
        fd_ = -1;
    }
}

//------------------------------------------------------------------------------
// epoll_sockets implementation
//------------------------------------------------------------------------------

inline
epoll_sockets::
epoll_sockets(capy::execution_context& ctx)
    : sched_(ctx.use_service<epoll_scheduler>())
{
}

inline
epoll_sockets::
~epoll_sockets()
{
}

inline void
epoll_sockets::
shutdown()
{
    std::lock_guard lock(mutex_);

    while (auto* impl = socket_list_.pop_front())
    {
        impl->close_socket();
        delete impl;
    }

    while (auto* impl = acceptor_list_.pop_front())
    {
        impl->close_socket();
        delete impl;
    }
}

inline epoll_socket_impl&
epoll_sockets::
create_impl()
{
    auto* impl = new epoll_socket_impl(*this);

    {
        std::lock_guard lock(mutex_);
        socket_list_.push_back(impl);
    }

    return *impl;
}

inline void
epoll_sockets::
destroy_impl(epoll_socket_impl& impl)
{
    {
        std::lock_guard lock(mutex_);
        socket_list_.remove(&impl);
    }

    delete &impl;
}

inline system::error_code
epoll_sockets::
open_socket(epoll_socket_impl& impl)
{
    impl.close_socket();

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return system::error_code(errno, system::system_category());

    impl.fd_ = fd;
    return {};
}

inline epoll_acceptor_impl&
epoll_sockets::
create_acceptor_impl()
{
    auto* impl = new epoll_acceptor_impl(*this);

    {
        std::lock_guard lock(mutex_);
        acceptor_list_.push_back(impl);
    }

    return *impl;
}

inline void
epoll_sockets::
destroy_acceptor_impl(epoll_acceptor_impl& impl)
{
    {
        std::lock_guard lock(mutex_);
        acceptor_list_.remove(&impl);
    }

    delete &impl;
}

inline system::error_code
epoll_sockets::
open_acceptor(
    epoll_acceptor_impl& impl,
    endpoint ep,
    int backlog)
{
    impl.close_socket();

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return system::error_code(errno, system::system_category());

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr = detail::to_sockaddr_in(ep);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        int err = errno;
        ::close(fd);
        return system::error_code(err, system::system_category());
    }

    if (::listen(fd, backlog) < 0)
    {
        int err = errno;
        ::close(fd);
        return system::error_code(err, system::system_category());
    }

    impl.fd_ = fd;
    return {};
}

inline void
epoll_sockets::
post(epoll_op* op)
{
    sched_.post(op);
}

inline void
epoll_sockets::
work_started() noexcept
{
    sched_.work_started();
}

inline void
epoll_sockets::
work_finished() noexcept
{
    sched_.work_finished();
}

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // BOOST_COROSIO_BACKEND_EPOLL

#endif // BOOST_COROSIO_DETAIL_EPOLL_SOCKETS_HPP
