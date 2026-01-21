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
#include <boost/capy/detail/intrusive.hpp>

#include "src/detail/epoll/op.hpp"
#include "src/detail/epoll/scheduler.hpp"
#include "src/detail/endpoint_convert.hpp"
#include "src/detail/make_err.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

/*
    epoll Socket Implementation
    ===========================

    Each I/O operation follows the same pattern:
      1. Try the syscall immediately (non-blocking socket)
      2. If it succeeds or fails with a real error, post to completion queue
      3. If EAGAIN/EWOULDBLOCK, register with epoll and wait

    This "try first" approach avoids unnecessary epoll round-trips for
    operations that can complete immediately (common for small reads/writes
    on fast local connections).

    One-Shot Registration
    ---------------------
    We use one-shot epoll registration: each operation registers, waits for
    one event, then unregisters. This simplifies the state machine since we
    don't need to track whether an fd is currently registered or handle
    re-arming. The tradeoff is slightly more epoll_ctl calls, but the
    simplicity is worth it.

    Cancellation
    ------------
    See op.hpp for the completion/cancellation race handling via the
    `registered` atomic. cancel() must complete pending operations (post
    them with cancelled flag) so coroutines waiting on them can resume.
    close_socket() calls cancel() first to ensure this.

    Impl Lifetime with shared_ptr
    -----------------------------
    Socket and acceptor impls use enable_shared_from_this. The service owns
    impls via shared_ptr vectors (socket_ptrs_, acceptor_ptrs_). When a user
    calls close(), we call cancel() which posts pending ops to the scheduler.

    CRITICAL: The posted ops must keep the impl alive until they complete.
    Otherwise the scheduler would process a freed op (use-after-free). The
    cancel() method captures shared_from_this() into op.impl_ptr before
    posting. When the op completes, impl_ptr is cleared, allowing the impl
    to be destroyed if no other references exist.

    The intrusive_list (socket_list_, acceptor_list_) provides fast iteration
    for shutdown cleanup. It stores raw pointers alongside the shared_ptr
    ownership in the vectors.

    Service Ownership
    -----------------
    epoll_sockets owns all socket impls. destroy_impl() removes the shared_ptr
    from the vector, but the impl may survive if ops still hold impl_ptr refs.
    shutdown() closes all sockets and clears the vectors; any in-flight ops
    will complete and release their refs, allowing final destruction.
*/

namespace boost {
namespace corosio {
namespace detail {

class epoll_sockets;
class epoll_socket_impl;
class epoll_acceptor_impl;

//------------------------------------------------------------------------------

class epoll_socket_impl
    : public socket::socket_impl
    , public std::enable_shared_from_this<epoll_socket_impl>
    , public capy::detail::intrusive_list<epoll_socket_impl>::node
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
        io_buffer_param,
        std::stop_token,
        system::error_code*,
        std::size_t*) override;

    void write_some(
        std::coroutine_handle<>,
        capy::any_executor_ref,
        io_buffer_param,
        std::stop_token,
        system::error_code*,
        std::size_t*) override;

    system::error_code shutdown(socket::shutdown_type what) noexcept override
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

class epoll_acceptor_impl
    : public acceptor::acceptor_impl
    , public std::enable_shared_from_this<epoll_acceptor_impl>
    , public capy::detail::intrusive_list<epoll_acceptor_impl>::node
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

class epoll_sockets
    : public capy::execution_context::service
{
public:
    using key_type = epoll_sockets;

    explicit epoll_sockets(capy::execution_context& ctx);
    ~epoll_sockets();

    epoll_sockets(epoll_sockets const&) = delete;
    epoll_sockets& operator=(epoll_sockets const&) = delete;

    void shutdown() override;

    epoll_socket_impl& create_impl();
    void destroy_impl(epoll_socket_impl& impl);
    system::error_code open_socket(epoll_socket_impl& impl);

    epoll_acceptor_impl& create_acceptor_impl();
    void destroy_acceptor_impl(epoll_acceptor_impl& impl);
    system::error_code open_acceptor(
        epoll_acceptor_impl& impl,
        endpoint ep,
        int backlog);

    epoll_scheduler& scheduler() const noexcept { return sched_; }
    void post(epoll_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

private:
    epoll_scheduler& sched_;
    std::mutex mutex_;

    // Dual tracking: intrusive_list for fast shutdown iteration,
    // vectors for shared_ptr ownership. See "Impl Lifetime" in file header.
    capy::detail::intrusive_list<epoll_socket_impl> socket_list_;
    capy::detail::intrusive_list<epoll_acceptor_impl> acceptor_list_;
    std::vector<std::shared_ptr<epoll_socket_impl>> socket_ptrs_;
    std::vector<std::shared_ptr<epoll_acceptor_impl>> acceptor_ptrs_;
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
        op.registered.store(true, std::memory_order_release);
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

inline void
epoll_socket_impl::
write_some(
    std::coroutine_handle<> h,
    capy::any_executor_ref d,
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

inline void
epoll_socket_impl::
cancel() noexcept
{
    std::shared_ptr<epoll_socket_impl> self;
    try {
        self = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return; // Not yet managed by shared_ptr (during construction)
    }

    auto cancel_op = [this, &self](epoll_op& op) {
        bool was_registered = op.registered.exchange(false, std::memory_order_acq_rel);
        op.request_cancel();
        if (was_registered)
        {
            svc_.scheduler().unregister_fd(fd_);
            op.impl_ptr = self; // prevent use-after-free
            svc_.post(&op);
            svc_.work_finished();
        }
    };

    cancel_op(conn_);
    cancel_op(rd_);
    cancel_op(wr_);
}

inline void
epoll_socket_impl::
close_socket() noexcept
{
    cancel();

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

    // Needed for deferred peer creation when accept completes via epoll path
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

inline void
epoll_acceptor_impl::
cancel() noexcept
{
    bool was_registered = acc_.registered.exchange(false, std::memory_order_acq_rel);
    acc_.request_cancel();

    if (was_registered)
    {
        svc_.scheduler().unregister_fd(fd_);
        try {
            acc_.impl_ptr = shared_from_this(); // prevent use-after-free
        } catch (const std::bad_weak_ptr&) {}
        svc_.post(&acc_);
        svc_.work_finished();
    }
}

inline void
epoll_acceptor_impl::
close_socket() noexcept
{
    cancel();

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
        impl->close_socket();

    while (auto* impl = acceptor_list_.pop_front())
        impl->close_socket();

    // Impls may outlive this if in-flight ops hold impl_ptr refs
    socket_ptrs_.clear();
    acceptor_ptrs_.clear();
}

inline epoll_socket_impl&
epoll_sockets::
create_impl()
{
    auto impl = std::make_shared<epoll_socket_impl>(*this);

    {
        std::lock_guard lock(mutex_);
        socket_list_.push_back(impl.get());
        socket_ptrs_.push_back(impl);
    }

    return *impl;
}

inline void
epoll_sockets::
destroy_impl(epoll_socket_impl& impl)
{
    std::lock_guard lock(mutex_);
    socket_list_.remove(&impl);

    // Impl may outlive this if pending ops hold impl_ptr refs
    auto it = std::find_if(socket_ptrs_.begin(), socket_ptrs_.end(),
        [&impl](const auto& ptr) { return ptr.get() == &impl; });
    if (it != socket_ptrs_.end())
        socket_ptrs_.erase(it);
}

inline system::error_code
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

inline epoll_acceptor_impl&
epoll_sockets::
create_acceptor_impl()
{
    auto impl = std::make_shared<epoll_acceptor_impl>(*this);

    {
        std::lock_guard lock(mutex_);
        acceptor_list_.push_back(impl.get());
        acceptor_ptrs_.push_back(impl);
    }

    return *impl;
}

inline void
epoll_sockets::
destroy_acceptor_impl(epoll_acceptor_impl& impl)
{
    std::lock_guard lock(mutex_);
    acceptor_list_.remove(&impl);

    auto it = std::find_if(acceptor_ptrs_.begin(), acceptor_ptrs_.end(),
        [&impl](const auto& ptr) { return ptr.get() == &impl; });
    if (it != acceptor_ptrs_.end())
        acceptor_ptrs_.erase(it);
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
