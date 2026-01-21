//
// Copyright (c) 2026 Cinar Gursoy
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_KQUEUE_SOCKETS_HPP
#define BOOST_COROSIO_DETAIL_KQUEUE_SOCKETS_HPP

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_KQUEUE)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/acceptor.hpp>
#include <boost/corosio/socket.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/concept/io_awaitable.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include "src/detail/intrusive.hpp"

#include "src/detail/kqueue/op.hpp"
#include "src/detail/kqueue/scheduler.hpp"
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
#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>

/*
    kqueue Socket Implementation
    ============================

    Each I/O operation follows the same pattern:
      1. Try the syscall immediately (non-blocking socket)
      2. If it succeeds or fails with a real error, post to completion queue
      3. If EAGAIN/EWOULDBLOCK, register with kqueue and wait

    This "try first" approach avoids unnecessary kqueue round-trips for
    operations that can complete immediately (common for small reads/writes
    on fast local connections).

    One-Shot Registration
    ---------------------
    We use one-shot kqueue registration (EV_ONESHOT): each operation registers,
    waits for one event, then the registration is automatically removed. This
    simplifies the state machine since we don't need to track whether an fd
    is currently registered or handle re-arming.

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

    macOS-specific Notes
    --------------------
    - No accept4(): Use accept() + fcntl() for NONBLOCK/CLOEXEC
    - No MSG_NOSIGNAL: Use SO_NOSIGPIPE socket option instead
    - Use writev() instead of sendmsg() for writes (simpler, works fine)
*/

namespace boost {
namespace corosio {
namespace detail {

class kqueue_sockets;
class kqueue_socket_impl;
class kqueue_acceptor_impl;

//------------------------------------------------------------------------------

class kqueue_socket_impl
    : public socket::socket_impl
    , public std::enable_shared_from_this<kqueue_socket_impl>
    , public intrusive_list<kqueue_socket_impl>::node
{
    friend class kqueue_sockets;

public:
    explicit kqueue_socket_impl(kqueue_sockets& svc) noexcept;

    void release() override;

    void connect(
        std::coroutine_handle<>,
        capy::executor_ref,
        endpoint,
        capy::stop_token,
        system::error_code*) override;

    void read_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        io_buffer_param,
        capy::stop_token,
        system::error_code*,
        std::size_t*) override;

    void write_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        io_buffer_param,
        capy::stop_token,
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

    kqueue_connect_op conn_;
    kqueue_read_op rd_;
    kqueue_write_op wr_;

private:
    kqueue_sockets& svc_;
    int fd_ = -1;
};

//------------------------------------------------------------------------------

class kqueue_acceptor_impl
    : public acceptor::acceptor_impl
    , public std::enable_shared_from_this<kqueue_acceptor_impl>
    , public intrusive_list<kqueue_acceptor_impl>::node
{
    friend class kqueue_sockets;

public:
    explicit kqueue_acceptor_impl(kqueue_sockets& svc) noexcept;

    void release() override;

    void accept(
        std::coroutine_handle<>,
        capy::executor_ref,
        capy::stop_token,
        system::error_code*,
        io_object::io_object_impl**) override;

    int native_handle() const noexcept { return fd_; }
    bool is_open() const noexcept { return fd_ >= 0; }
    void cancel() noexcept;
    void close_socket() noexcept;

    kqueue_accept_op acc_;

private:
    kqueue_sockets& svc_;
    int fd_ = -1;
};

//------------------------------------------------------------------------------

class kqueue_sockets
    : public capy::execution_context::service
{
public:
    using key_type = kqueue_sockets;

    explicit kqueue_sockets(capy::execution_context& ctx);
    ~kqueue_sockets();

    kqueue_sockets(kqueue_sockets const&) = delete;
    kqueue_sockets& operator=(kqueue_sockets const&) = delete;

    void shutdown() override;

    kqueue_socket_impl& create_impl();
    void destroy_impl(kqueue_socket_impl& impl);
    system::error_code open_socket(kqueue_socket_impl& impl);

    kqueue_acceptor_impl& create_acceptor_impl();
    void destroy_acceptor_impl(kqueue_acceptor_impl& impl);
    system::error_code open_acceptor(
        kqueue_acceptor_impl& impl,
        endpoint ep,
        int backlog);

    kqueue_scheduler& scheduler() const noexcept { return sched_; }
    void post(kqueue_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

private:
    kqueue_scheduler& sched_;
    std::mutex mutex_;

    // Dual tracking: intrusive_list for fast shutdown iteration,
    // vectors for shared_ptr ownership. See "Impl Lifetime" in file header.
    intrusive_list<kqueue_socket_impl> socket_list_;
    intrusive_list<kqueue_acceptor_impl> acceptor_list_;
    std::vector<std::shared_ptr<kqueue_socket_impl>> socket_ptrs_;
    std::vector<std::shared_ptr<kqueue_acceptor_impl>> acceptor_ptrs_;
};

//------------------------------------------------------------------------------
// kqueue_socket_impl implementation
//------------------------------------------------------------------------------

inline
kqueue_socket_impl::
kqueue_socket_impl(kqueue_sockets& svc) noexcept
    : svc_(svc)
{
}

inline void
kqueue_socket_impl::
release()
{
    close_socket();
    svc_.destroy_impl(*this);
}

inline void
kqueue_socket_impl::
connect(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    endpoint ep,
    capy::stop_token token,
    system::error_code* ec)
{
    auto& op = conn_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.fd = fd_;
    op.filter = EVFILT_WRITE;
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
        svc_.scheduler().register_fd(fd_, &op, EVFILT_WRITE);
        return;
    }

    op.complete(errno, 0);
    svc_.post(&op);
}

inline void
kqueue_socket_impl::
read_some(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    io_buffer_param param,
    capy::stop_token token,
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
    op.filter = EVFILT_READ;
    op.start(token);

    capy::mutable_buffer bufs[kqueue_read_op::max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, kqueue_read_op::max_buffers));

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
        svc_.scheduler().register_fd(fd_, &op, EVFILT_READ);
        return;
    }

    op.complete(errno, 0);
    svc_.post(&op);
}

inline void
kqueue_socket_impl::
write_some(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    io_buffer_param param,
    capy::stop_token token,
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
    op.filter = EVFILT_WRITE;
    op.start(token);

    capy::mutable_buffer bufs[kqueue_write_op::max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, kqueue_write_op::max_buffers));

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

    // On macOS, SO_NOSIGPIPE is set on the socket, so we can use writev
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
        op.registered.store(true, std::memory_order_release);
        svc_.scheduler().register_fd(fd_, &op, EVFILT_WRITE);
        return;
    }

    op.complete(errno ? errno : EIO, 0);
    svc_.post(&op);
}

inline void
kqueue_socket_impl::
cancel() noexcept
{
    std::shared_ptr<kqueue_socket_impl> self;
    try {
        self = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return; // Not yet managed by shared_ptr (during construction)
    }

    auto cancel_op = [this, &self](kqueue_op& op) {
        bool was_registered = op.registered.exchange(false, std::memory_order_acq_rel);
        op.request_cancel();
        if (was_registered)
        {
            svc_.scheduler().unregister_fd(fd_, op.filter);
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
kqueue_socket_impl::
close_socket() noexcept
{
    cancel();

    if (fd_ >= 0)
    {
        // Unregister both filters just in case
        svc_.scheduler().unregister_fd(fd_, EVFILT_READ);
        svc_.scheduler().unregister_fd(fd_, EVFILT_WRITE);
        ::close(fd_);
        fd_ = -1;
    }
}

//------------------------------------------------------------------------------
// kqueue_acceptor_impl implementation
//------------------------------------------------------------------------------

inline
kqueue_acceptor_impl::
kqueue_acceptor_impl(kqueue_sockets& svc) noexcept
    : svc_(svc)
{
}

inline void
kqueue_acceptor_impl::
release()
{
    close_socket();
    svc_.destroy_acceptor_impl(*this);
}

inline void
kqueue_acceptor_impl::
accept(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    capy::stop_token token,
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
    op.filter = EVFILT_READ;
    op.start(token);

    // Needed for deferred peer creation when accept completes via kqueue path
    op.service_ptr = &svc_;
    op.create_peer = [](void* svc_ptr, int new_fd) -> io_object::io_object_impl* {
        auto& svc = *static_cast<kqueue_sockets*>(svc_ptr);
        auto& peer_impl = svc.create_impl();
        peer_impl.set_socket(new_fd);
        return &peer_impl;
    };

    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    // macOS doesn't have accept4, use accept and set flags via fcntl
    int accepted = ::accept(fd_, reinterpret_cast<sockaddr*>(&addr), &addrlen);

    if (accepted >= 0)
    {
        // Set non-blocking
        int flags = ::fcntl(accepted, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(accepted, F_SETFL, flags | O_NONBLOCK);

        // Set close-on-exec
        flags = ::fcntl(accepted, F_GETFD, 0);
        if (flags >= 0)
            ::fcntl(accepted, F_SETFD, flags | FD_CLOEXEC);

        // Set SO_NOSIGPIPE
        int nosigpipe = 1;
        ::setsockopt(accepted, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));

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
        svc_.scheduler().register_fd(fd_, &op, EVFILT_READ);
        return;
    }

    op.complete(errno, 0);
    svc_.post(&op);
}

inline void
kqueue_acceptor_impl::
cancel() noexcept
{
    bool was_registered = acc_.registered.exchange(false, std::memory_order_acq_rel);
    acc_.request_cancel();

    if (was_registered)
    {
        svc_.scheduler().unregister_fd(fd_, EVFILT_READ);
        try {
            acc_.impl_ptr = shared_from_this(); // prevent use-after-free
        } catch (const std::bad_weak_ptr&) {}
        svc_.post(&acc_);
        svc_.work_finished();
    }
}

inline void
kqueue_acceptor_impl::
close_socket() noexcept
{
    cancel();

    if (fd_ >= 0)
    {
        svc_.scheduler().unregister_fd(fd_, EVFILT_READ);
        ::close(fd_);
        fd_ = -1;
    }
}

//------------------------------------------------------------------------------
// kqueue_sockets implementation
//------------------------------------------------------------------------------

inline
kqueue_sockets::
kqueue_sockets(capy::execution_context& ctx)
    : sched_(ctx.use_service<kqueue_scheduler>())
{
}

inline
kqueue_sockets::
~kqueue_sockets()
{
}

inline void
kqueue_sockets::
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

inline kqueue_socket_impl&
kqueue_sockets::
create_impl()
{
    auto impl = std::make_shared<kqueue_socket_impl>(*this);

    {
        std::lock_guard lock(mutex_);
        socket_list_.push_back(impl.get());
        socket_ptrs_.push_back(impl);
    }

    return *impl;
}

inline void
kqueue_sockets::
destroy_impl(kqueue_socket_impl& impl)
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
kqueue_sockets::
open_socket(kqueue_socket_impl& impl)
{
    impl.close_socket();

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return make_err(errno);

    // Set non-blocking
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }

    // Set close-on-exec
    flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);

    // Set SO_NOSIGPIPE to prevent SIGPIPE on write to closed socket
    int nosigpipe = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));

    impl.fd_ = fd;
    return {};
}

inline kqueue_acceptor_impl&
kqueue_sockets::
create_acceptor_impl()
{
    auto impl = std::make_shared<kqueue_acceptor_impl>(*this);

    {
        std::lock_guard lock(mutex_);
        acceptor_list_.push_back(impl.get());
        acceptor_ptrs_.push_back(impl);
    }

    return *impl;
}

inline void
kqueue_sockets::
destroy_acceptor_impl(kqueue_acceptor_impl& impl)
{
    std::lock_guard lock(mutex_);
    acceptor_list_.remove(&impl);

    auto it = std::find_if(acceptor_ptrs_.begin(), acceptor_ptrs_.end(),
        [&impl](const auto& ptr) { return ptr.get() == &impl; });
    if (it != acceptor_ptrs_.end())
        acceptor_ptrs_.erase(it);
}

inline system::error_code
kqueue_sockets::
open_acceptor(
    kqueue_acceptor_impl& impl,
    endpoint ep,
    int backlog)
{
    impl.close_socket();

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return make_err(errno);

    // Set non-blocking
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }

    // Set close-on-exec
    flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);

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
kqueue_sockets::
post(kqueue_op* op)
{
    sched_.post(op);
}

inline void
kqueue_sockets::
work_started() noexcept
{
    sched_.work_started();
}

inline void
kqueue_sockets::
work_finished() noexcept
{
    sched_.work_finished();
}

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // BOOST_COROSIO_BACKEND_KQUEUE

#endif // BOOST_COROSIO_DETAIL_KQUEUE_SOCKETS_HPP
