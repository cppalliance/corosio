//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_SOCKET_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_SOCKET_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/detail/socket_service.hpp>

#include <boost/corosio/native/detail/select/select_socket.hpp>
#include <boost/corosio/native/detail/select/select_scheduler.hpp>

#include <boost/corosio/detail/endpoint_convert.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/detail/make_err.hpp>

#include <boost/corosio/detail/except.hpp>

#include <boost/capy/buffers.hpp>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <memory>
#include <mutex>
#include <unordered_map>

/*
    select Socket Implementation
    ============================

    This mirrors the epoll_sockets design for behavioral consistency.
    Each I/O operation follows the same pattern:
      1. Try the syscall immediately (non-blocking socket)
      2. If it succeeds or fails with a real error, post to completion queue
      3. If EAGAIN/EWOULDBLOCK, register with select scheduler and wait

    Cancellation
    ------------
    See op.hpp for the completion/cancellation race handling via the
    `registered` atomic. cancel() must complete pending operations (post
    them with cancelled flag) so coroutines waiting on them can resume.
    close_socket() calls cancel() first to ensure this.

    Impl Lifetime with shared_ptr
    -----------------------------
    Socket impls use enable_shared_from_this. The service owns impls via
    shared_ptr maps (socket_ptrs_) keyed by raw pointer for O(1) lookup and
    removal. When a user calls close(), we call cancel() which posts pending
    ops to the scheduler.

    CRITICAL: The posted ops must keep the impl alive until they complete.
    Otherwise the scheduler would process a freed op (use-after-free). The
    cancel() method captures shared_from_this() into op.impl_ptr before
    posting. When the op completes, impl_ptr is cleared, allowing the impl
    to be destroyed if no other references exist.

    Service Ownership
    -----------------
    select_socket_service owns all socket impls. destroy() removes the
    shared_ptr from the map, but the impl may survive if ops still hold
    impl_ptr refs. shutdown() closes all sockets and clears the map; any
    in-flight ops will complete and release their refs.
*/

namespace boost::corosio::detail {

/** State for select socket service. */
class select_socket_state
{
public:
    explicit select_socket_state(select_scheduler& sched) noexcept
        : sched_(sched)
    {
    }

    select_scheduler& sched_;
    std::mutex mutex_;
    intrusive_list<select_socket> socket_list_;
    std::unordered_map<select_socket*, std::shared_ptr<select_socket>>
        socket_ptrs_;
};

/** select socket service implementation.

    Inherits from socket_service to enable runtime polymorphism.
    Uses key_type = socket_service for service lookup.
*/
class BOOST_COROSIO_DECL select_socket_service final : public socket_service
{
public:
    explicit select_socket_service(capy::execution_context& ctx);
    ~select_socket_service() override;

    select_socket_service(select_socket_service const&)            = delete;
    select_socket_service& operator=(select_socket_service const&) = delete;

    void shutdown() override;

    io_object::implementation* construct() override;
    void destroy(io_object::implementation*) override;
    void close(io_object::handle&) override;
    std::error_code open_socket(tcp_socket::implementation& impl) override;

    select_scheduler& scheduler() const noexcept
    {
        return state_->sched_;
    }
    void post(select_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

private:
    std::unique_ptr<select_socket_state> state_;
};

// Backward compatibility alias
using select_sockets = select_socket_service;

inline void
select_op::canceller::operator()() const noexcept
{
    op->cancel();
}

inline void
select_connect_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
select_read_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
select_write_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
select_connect_op::operator()()
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
        if (::getsockname(
                fd, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
            local_ep = from_sockaddr_in(local_addr);
        // Always cache remote endpoint; local may be default if getsockname failed
        static_cast<select_socket*>(socket_impl_)
            ->set_endpoints(local_ep, target_endpoint);
    }

    if (ec_out)
    {
        if (cancelled.load(std::memory_order_acquire))
            *ec_out = capy::error::canceled;
        else if (errn != 0)
            *ec_out = make_err(errn);
        else
            *ec_out = {};
    }

    if (bytes_out)
        *bytes_out = bytes_transferred;

    // Move to stack before destroying the frame
    capy::executor_ref saved_ex(ex);
    std::coroutine_handle<> saved_h(h);
    impl_ptr.reset();
    dispatch_coro(saved_ex, saved_h).resume();
}

inline select_socket::select_socket(select_socket_service& svc) noexcept
    : svc_(svc)
{
}

inline std::coroutine_handle<>
select_socket::connect(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    endpoint ep,
    std::stop_token token,
    std::error_code* ec)
{
    auto& op = conn_;
    op.reset();
    op.h               = h;
    op.ex              = ex;
    op.ec_out          = ec;
    op.fd              = fd_;
    op.target_endpoint = ep; // Store target for endpoint caching
    op.start(token, this);

    sockaddr_in addr = detail::to_sockaddr_in(ep);
    int result =
        ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    if (result == 0)
    {
        // Sync success - cache endpoints immediately
        sockaddr_in local_addr{};
        socklen_t local_len = sizeof(local_addr);
        if (::getsockname(
                fd_, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
            local_endpoint_ = detail::from_sockaddr_in(local_addr);
        remote_endpoint_ = ep;

        op.complete(0, 0);
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        // completion is always posted to scheduler queue, never inline.
        return std::noop_coroutine();
    }

    if (errno == EINPROGRESS)
    {
        svc_.work_started();
        op.impl_ptr = shared_from_this();

        // Set registering BEFORE register_fd to close the race window where
        // reactor sees an event before we set registered. The reactor treats
        // registering the same as registered when claiming the op.
        op.registered.store(
            select_registration_state::registering, std::memory_order_release);
        svc_.scheduler().register_fd(fd_, &op, select_scheduler::event_write);

        // Transition to registered. If this fails, reactor or cancel already
        // claimed the op (state is now unregistered), so we're done. However,
        // we must still deregister the fd because cancel's deregister_fd may
        // have run before our register_fd, leaving the fd orphaned.
        auto expected = select_registration_state::registering;
        if (!op.registered.compare_exchange_strong(
                expected, select_registration_state::registered,
                std::memory_order_acq_rel))
        {
            svc_.scheduler().deregister_fd(fd_, select_scheduler::event_write);
            // completion is always posted to scheduler queue, never inline.
            return std::noop_coroutine();
        }

        // If cancelled was set before we registered, handle it now.
        if (op.cancelled.load(std::memory_order_acquire))
        {
            auto prev = op.registered.exchange(
                select_registration_state::unregistered,
                std::memory_order_acq_rel);
            if (prev != select_registration_state::unregistered)
            {
                svc_.scheduler().deregister_fd(
                    fd_, select_scheduler::event_write);
                op.impl_ptr = shared_from_this();
                svc_.post(&op);
                svc_.work_finished();
            }
        }
        // completion is always posted to scheduler queue, never inline.
        return std::noop_coroutine();
    }

    op.complete(errno, 0);
    op.impl_ptr = shared_from_this();
    svc_.post(&op);
    // completion is always posted to scheduler queue, never inline.
    return std::noop_coroutine();
}

inline std::coroutine_handle<>
select_socket::read_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    io_buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = rd_;
    op.reset();
    op.h         = h;
    op.ex        = ex;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.fd        = fd_;
    op.start(token, this);

    capy::mutable_buffer bufs[select_read_op::max_buffers];
    op.iovec_count =
        static_cast<int>(param.copy_to(bufs, select_read_op::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.empty_buffer_read = true;
        op.complete(0, 0);
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return std::noop_coroutine();
    }

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len  = bufs[i].size();
    }

    ssize_t n = ::readv(fd_, op.iovecs, op.iovec_count);

    if (n > 0)
    {
        op.complete(0, static_cast<std::size_t>(n));
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return std::noop_coroutine();
    }

    if (n == 0)
    {
        op.complete(0, 0);
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return std::noop_coroutine();
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        svc_.work_started();
        op.impl_ptr = shared_from_this();

        // Set registering BEFORE register_fd to close the race window where
        // reactor sees an event before we set registered.
        op.registered.store(
            select_registration_state::registering, std::memory_order_release);
        svc_.scheduler().register_fd(fd_, &op, select_scheduler::event_read);

        // Transition to registered. If this fails, reactor or cancel already
        // claimed the op (state is now unregistered), so we're done. However,
        // we must still deregister the fd because cancel's deregister_fd may
        // have run before our register_fd, leaving the fd orphaned.
        auto expected = select_registration_state::registering;
        if (!op.registered.compare_exchange_strong(
                expected, select_registration_state::registered,
                std::memory_order_acq_rel))
        {
            svc_.scheduler().deregister_fd(fd_, select_scheduler::event_read);
            return std::noop_coroutine();
        }

        // If cancelled was set before we registered, handle it now.
        if (op.cancelled.load(std::memory_order_acquire))
        {
            auto prev = op.registered.exchange(
                select_registration_state::unregistered,
                std::memory_order_acq_rel);
            if (prev != select_registration_state::unregistered)
            {
                svc_.scheduler().deregister_fd(
                    fd_, select_scheduler::event_read);
                op.impl_ptr = shared_from_this();
                svc_.post(&op);
                svc_.work_finished();
            }
        }
        return std::noop_coroutine();
    }

    op.complete(errno, 0);
    op.impl_ptr = shared_from_this();
    svc_.post(&op);
    return std::noop_coroutine();
}

inline std::coroutine_handle<>
select_socket::write_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    io_buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = wr_;
    op.reset();
    op.h         = h;
    op.ex        = ex;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.fd        = fd_;
    op.start(token, this);

    capy::mutable_buffer bufs[select_write_op::max_buffers];
    op.iovec_count =
        static_cast<int>(param.copy_to(bufs, select_write_op::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.complete(0, 0);
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return std::noop_coroutine();
    }

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len  = bufs[i].size();
    }

    msghdr msg{};
    msg.msg_iov    = op.iovecs;
    msg.msg_iovlen = static_cast<std::size_t>(op.iovec_count);

    ssize_t n = ::sendmsg(fd_, &msg, MSG_NOSIGNAL);

    if (n > 0)
    {
        op.complete(0, static_cast<std::size_t>(n));
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return std::noop_coroutine();
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        svc_.work_started();
        op.impl_ptr = shared_from_this();

        // Set registering BEFORE register_fd to close the race window where
        // reactor sees an event before we set registered.
        op.registered.store(
            select_registration_state::registering, std::memory_order_release);
        svc_.scheduler().register_fd(fd_, &op, select_scheduler::event_write);

        // Transition to registered. If this fails, reactor or cancel already
        // claimed the op (state is now unregistered), so we're done. However,
        // we must still deregister the fd because cancel's deregister_fd may
        // have run before our register_fd, leaving the fd orphaned.
        auto expected = select_registration_state::registering;
        if (!op.registered.compare_exchange_strong(
                expected, select_registration_state::registered,
                std::memory_order_acq_rel))
        {
            svc_.scheduler().deregister_fd(fd_, select_scheduler::event_write);
            return std::noop_coroutine();
        }

        // If cancelled was set before we registered, handle it now.
        if (op.cancelled.load(std::memory_order_acquire))
        {
            auto prev = op.registered.exchange(
                select_registration_state::unregistered,
                std::memory_order_acq_rel);
            if (prev != select_registration_state::unregistered)
            {
                svc_.scheduler().deregister_fd(
                    fd_, select_scheduler::event_write);
                op.impl_ptr = shared_from_this();
                svc_.post(&op);
                svc_.work_finished();
            }
        }
        return std::noop_coroutine();
    }

    op.complete(errno ? errno : EIO, 0);
    op.impl_ptr = shared_from_this();
    svc_.post(&op);
    return std::noop_coroutine();
}

inline std::error_code
select_socket::shutdown(tcp_socket::shutdown_type what) noexcept
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

inline std::error_code
select_socket::set_no_delay(bool value) noexcept
{
    int flag = value ? 1 : 0;
    if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) != 0)
        return make_err(errno);
    return {};
}

inline bool
select_socket::no_delay(std::error_code& ec) const noexcept
{
    int flag      = 0;
    socklen_t len = sizeof(flag);
    if (::getsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, &len) != 0)
    {
        ec = make_err(errno);
        return false;
    }
    ec = {};
    return flag != 0;
}

inline std::error_code
select_socket::set_keep_alive(bool value) noexcept
{
    int flag = value ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag)) != 0)
        return make_err(errno);
    return {};
}

inline bool
select_socket::keep_alive(std::error_code& ec) const noexcept
{
    int flag      = 0;
    socklen_t len = sizeof(flag);
    if (::getsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &flag, &len) != 0)
    {
        ec = make_err(errno);
        return false;
    }
    ec = {};
    return flag != 0;
}

inline std::error_code
select_socket::set_receive_buffer_size(int size) noexcept
{
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) != 0)
        return make_err(errno);
    return {};
}

inline int
select_socket::receive_buffer_size(std::error_code& ec) const noexcept
{
    int size      = 0;
    socklen_t len = sizeof(size);
    if (::getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, &len) != 0)
    {
        ec = make_err(errno);
        return 0;
    }
    ec = {};
    return size;
}

inline std::error_code
select_socket::set_send_buffer_size(int size) noexcept
{
    if (::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) != 0)
        return make_err(errno);
    return {};
}

inline int
select_socket::send_buffer_size(std::error_code& ec) const noexcept
{
    int size      = 0;
    socklen_t len = sizeof(size);
    if (::getsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, &len) != 0)
    {
        ec = make_err(errno);
        return 0;
    }
    ec = {};
    return size;
}

inline std::error_code
select_socket::set_linger(bool enabled, int timeout) noexcept
{
    if (timeout < 0)
        return make_err(EINVAL);
    struct ::linger lg;
    lg.l_onoff  = enabled ? 1 : 0;
    lg.l_linger = timeout;
    if (::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) != 0)
        return make_err(errno);
    return {};
}

inline tcp_socket::linger_options
select_socket::linger(std::error_code& ec) const noexcept
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

inline void
select_socket::cancel() noexcept
{
    auto self = weak_from_this().lock();
    if (!self)
        return;

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

inline void
select_socket::cancel_single_op(select_op& op) noexcept
{
    auto self = weak_from_this().lock();
    if (!self)
        return;

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

        op.impl_ptr = self;
        svc_.post(&op);
        svc_.work_finished();
    }
}

inline void
select_socket::close_socket() noexcept
{
    auto self = weak_from_this().lock();
    if (self)
    {
        auto cancel_op = [this, &self](select_op& op, int events) {
            auto prev = op.registered.exchange(
                select_registration_state::unregistered,
                std::memory_order_acq_rel);
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

    if (fd_ >= 0)
    {
        svc_.scheduler().deregister_fd(
            fd_, select_scheduler::event_read | select_scheduler::event_write);
        ::close(fd_);
        fd_ = -1;
    }

    local_endpoint_  = endpoint{};
    remote_endpoint_ = endpoint{};
}

inline select_socket_service::select_socket_service(
    capy::execution_context& ctx)
    : state_(
          std::make_unique<select_socket_state>(
              ctx.use_service<select_scheduler>()))
{
}

inline select_socket_service::~select_socket_service() {}

inline void
select_socket_service::shutdown()
{
    std::lock_guard lock(state_->mutex_);

    while (auto* impl = state_->socket_list_.pop_front())
        impl->close_socket();

    // Don't clear socket_ptrs_ here. The scheduler shuts down after us and
    // drains completed_ops_, calling destroy() on each queued op. Letting
    // ~state_ release the ptrs (during service destruction, after scheduler
    // shutdown) keeps every impl alive until all ops have been drained.
}

inline io_object::implementation*
select_socket_service::construct()
{
    auto impl = std::make_shared<select_socket>(*this);
    auto* raw = impl.get();

    {
        std::lock_guard lock(state_->mutex_);
        state_->socket_list_.push_back(raw);
        state_->socket_ptrs_.emplace(raw, std::move(impl));
    }

    return raw;
}

inline void
select_socket_service::destroy(io_object::implementation* impl)
{
    auto* select_impl = static_cast<select_socket*>(impl);
    select_impl->close_socket();
    std::lock_guard lock(state_->mutex_);
    state_->socket_list_.remove(select_impl);
    state_->socket_ptrs_.erase(select_impl);
}

inline std::error_code
select_socket_service::open_socket(tcp_socket::implementation& impl)
{
    auto* select_impl = static_cast<select_socket*>(&impl);
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
        return make_err(EMFILE); // Too many open files
    }

    select_impl->fd_ = fd;
    return {};
}

inline void
select_socket_service::close(io_object::handle& h)
{
    static_cast<select_socket*>(h.get())->close_socket();
}

inline void
select_socket_service::post(select_op* op)
{
    state_->sched_.post(op);
}

inline void
select_socket_service::work_started() noexcept
{
    state_->sched_.work_started();
}

inline void
select_socket_service::work_finished() noexcept
{
    state_->sched_.work_finished();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_SOCKET_SERVICE_HPP
