//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include "src/detail/kqueue/sockets.hpp"
#include "src/detail/endpoint_convert.hpp"
#include "src/detail/make_err.hpp"
#include "src/detail/dispatch_coro.hpp"

#include <boost/corosio/detail/except.hpp>
#include <boost/capy/buffers.hpp>

#include <utility>

/*
    kqueue socket implementation
    ============================

    Each kqueue_socket_impl owns a descriptor_state that is persistently
    registered with kqueue (EVFILT_READ + EVFILT_WRITE, both EV_CLEAR for
    edge-triggered semantics). The descriptor_state tracks three operation
    slots (read_op, write_op, connect_op) and two ready flags
    (read_ready, write_ready) under a per-descriptor mutex.

    Ready-flag protocol
    -------------------
    When a kqueue event fires and no operation is pending for that
    direction, the reactor sets the corresponding ready flag instead of
    dropping the event. When a new operation starts and finds the ready
    flag set, it performs I/O immediately rather than parking in the
    descriptor_state slot. This prevents lost wakeups under edge-triggered
    notification.

    Edge-triggered retry
    --------------------
    Because EV_CLEAR delivers each transition exactly once, a single
    event may correspond to more data than one I/O call can consume. The
    retry loops in connect(), do_read_io(), and do_write_io() repeat
    perform_io() while EAGAIN/EWOULDBLOCK is returned and the ready flag
    has been re-set. When the flag is clear the operation parks in its
    descriptor_state slot and waits for the next kqueue event.

    Symmetric transfer and the cached_initiator
    --------------------------------------------
    read_some() and write_some() return a coroutine_handle<> for symmetric
    transfer so the caller is fully suspended before any I/O is attempted.
    The cached_initiator manages a reusable coroutine frame that calls
    do_read_io / do_write_io after the caller suspends. This avoids a
    heap allocation per operation and guarantees the caller's state is
    consistent if a cancellation races with completion.
*/

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

void
kqueue_op::canceller::
operator()() const noexcept
{
    op->cancel();
}

void
kqueue_connect_op::
cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

void
kqueue_read_op::
cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

void
kqueue_write_op::
cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

void
kqueue_connect_op::
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
        static_cast<kqueue_socket_impl*>(socket_impl_)->set_endpoints(local_ep, target_endpoint);
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

    // Move to stack before resuming. See kqueue_op::operator()() for rationale.
    capy::executor_ref saved_ex( std::move( ex ) );
    std::coroutine_handle<> saved_h( std::move( h ) );
    auto prevent_premature_destruction = std::move(impl_ptr);
    dispatch_coro(saved_ex, saved_h).resume();
}

kqueue_socket_impl::
kqueue_socket_impl(kqueue_socket_service& svc) noexcept
    : svc_(svc)
{
}

kqueue_socket_impl::
~kqueue_socket_impl() = default;

void
kqueue_socket_impl::
release()
{
    close_socket();
    svc_.destroy_impl(*this);
}

std::coroutine_handle<>
kqueue_socket_impl::
connect(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    endpoint ep,
    std::stop_token token,
    std::error_code* ec)
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
        sockaddr_in local_addr{};
        socklen_t local_len = sizeof(local_addr);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
            local_endpoint_ = detail::from_sockaddr_in(local_addr);
        remote_endpoint_ = ep;

        op.complete(0, 0);
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return std::noop_coroutine();
    }

    if (errno == EINPROGRESS)
    {
        svc_.work_started();
        op.impl_ptr = shared_from_this();

        bool perform_now = false;
        {
            std::lock_guard lock(desc_state_.mutex);
            if (desc_state_.write_ready)
            {
                desc_state_.write_ready = false;
                perform_now = true;
            }
            else
            {
                desc_state_.connect_op = &op;
            }
        }

        if (perform_now)
        {
            for (;;)
            {
                op.perform_io();
                if (op.errn != EAGAIN && op.errn != EWOULDBLOCK)
                {
                    svc_.post(&op);
                    svc_.work_finished();
                    break;
                }
                op.errn = 0;
                std::lock_guard lock(desc_state_.mutex);
                if (desc_state_.write_ready)
                {
                    desc_state_.write_ready = false;
                    continue;
                }
                desc_state_.connect_op = &op;
                break;
            }
            return std::noop_coroutine();
        }

        if (op.cancelled.load(std::memory_order_acquire))
        {
            kqueue_op* claimed = nullptr;
            {
                std::lock_guard lock(desc_state_.mutex);
                if (desc_state_.connect_op == &op)
                    claimed = std::exchange(desc_state_.connect_op, nullptr);
            }
            if (claimed)
            {
                svc_.post(claimed);
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

void
kqueue_socket_impl::
do_read_io()
{
    auto& op = rd_;

    ssize_t n = ::readv(fd_, op.iovecs, op.iovec_count);

    if (n > 0)
    {
        {
            std::lock_guard lock(desc_state_.mutex);
            desc_state_.read_ready = false;
        }
        op.complete(0, static_cast<std::size_t>(n));
        svc_.post(&op);
        return;
    }

    if (n == 0)
    {
        {
            std::lock_guard lock(desc_state_.mutex);
            desc_state_.read_ready = false;
        }
        op.complete(0, 0);
        svc_.post(&op);
        return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        svc_.work_started();

        bool perform_now = false;
        {
            std::lock_guard lock(desc_state_.mutex);
            if (desc_state_.read_ready)
            {
                desc_state_.read_ready = false;
                perform_now = true;
            }
            else
            {
                desc_state_.read_op = &op;
            }
        }

        if (perform_now)
        {
            for (;;)
            {
                op.perform_io();
                if (op.errn != EAGAIN && op.errn != EWOULDBLOCK)
                {
                    svc_.post(&op);
                    svc_.work_finished();
                    return;
                }
                op.errn = 0;
                std::lock_guard lock(desc_state_.mutex);
                if (desc_state_.read_ready)
                {
                    desc_state_.read_ready = false;
                    continue;
                }
                desc_state_.read_op = &op;
                break;
            }
            return;
        }

        if (op.cancelled.load(std::memory_order_acquire))
        {
            kqueue_op* claimed = nullptr;
            {
                std::lock_guard lock(desc_state_.mutex);
                if (desc_state_.read_op == &op)
                    claimed = std::exchange(desc_state_.read_op, nullptr);
            }
            if (claimed)
            {
                svc_.post(claimed);
                svc_.work_finished();
            }
        }
        return;
    }

    op.complete(errno, 0);
    svc_.post(&op);
}

void
kqueue_socket_impl::
do_write_io()
{
    auto& op = wr_;

    // SO_NOSIGPIPE is set on the socket at creation time, so writev() is safe.
    // FreeBSD: Supports MSG_NOSIGNAL on sendmsg()
    ssize_t n = ::writev(fd_, op.iovecs, op.iovec_count);

    if (n >= 0)
    {
        {
            std::lock_guard lock(desc_state_.mutex);
            desc_state_.write_ready = false;
        }
        op.complete(0, static_cast<std::size_t>(n));
        svc_.post(&op);
        return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        svc_.work_started();

        bool perform_now = false;
        {
            std::lock_guard lock(desc_state_.mutex);
            if (desc_state_.write_ready)
            {
                desc_state_.write_ready = false;
                perform_now = true;
            }
            else
            {
                desc_state_.write_op = &op;
            }
        }

        if (perform_now)
        {
            for (;;)
            {
                op.perform_io();
                if (op.errn != EAGAIN && op.errn != EWOULDBLOCK)
                {
                    svc_.post(&op);
                    svc_.work_finished();
                    return;
                }
                op.errn = 0;
                std::lock_guard lock(desc_state_.mutex);
                if (desc_state_.write_ready)
                {
                    desc_state_.write_ready = false;
                    continue;
                }
                desc_state_.write_op = &op;
                break;
            }
            return;
        }

        if (op.cancelled.load(std::memory_order_acquire))
        {
            kqueue_op* claimed = nullptr;
            {
                std::lock_guard lock(desc_state_.mutex);
                if (desc_state_.write_op == &op)
                    claimed = std::exchange(desc_state_.write_op, nullptr);
            }
            if (claimed)
            {
                svc_.post(claimed);
                svc_.work_finished();
            }
        }
        return;
    }

    op.complete(errno, 0);
    svc_.post(&op);
}

std::coroutine_handle<>
kqueue_socket_impl::
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
    op.h = h;
    op.ex = ex;
    op.ec_out = ec;
    op.bytes_out = bytes_out;
    op.fd = fd_;
    op.start(token, this);
    op.impl_ptr = shared_from_this();

    // Must prepare buffers before initiator runs
    capy::mutable_buffer bufs[kqueue_read_op::max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, kqueue_read_op::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.empty_buffer_read = true;
        op.complete(0, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len = bufs[i].size();
    }

    // Symmetric transfer ensures caller is suspended before I/O starts
    return read_initiator_.start<&kqueue_socket_impl::do_read_io>(this);
}

std::coroutine_handle<>
kqueue_socket_impl::
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
    op.h = h;
    op.ex = ex;
    op.ec_out = ec;
    op.bytes_out = bytes_out;
    op.fd = fd_;
    op.start(token, this);
    op.impl_ptr = shared_from_this();

    // Must prepare buffers before initiator runs
    capy::mutable_buffer bufs[kqueue_write_op::max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, kqueue_write_op::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.complete(0, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len = bufs[i].size();
    }

    // Symmetric transfer ensures caller is suspended before I/O starts
    return write_initiator_.start<&kqueue_socket_impl::do_write_io>(this);
}

std::error_code
kqueue_socket_impl::
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
kqueue_socket_impl::
set_no_delay(bool value) noexcept
{
    int flag = value ? 1 : 0;
    if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) != 0)
        return make_err(errno);
    return {};
}

bool
kqueue_socket_impl::
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
kqueue_socket_impl::
set_keep_alive(bool value) noexcept
{
    int flag = value ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag)) != 0)
        return make_err(errno);
    return {};
}

bool
kqueue_socket_impl::
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
kqueue_socket_impl::
set_receive_buffer_size(int size) noexcept
{
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) != 0)
        return make_err(errno);
    return {};
}

int
kqueue_socket_impl::
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
kqueue_socket_impl::
set_send_buffer_size(int size) noexcept
{
    if (::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) != 0)
        return make_err(errno);
    return {};
}

int
kqueue_socket_impl::
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
kqueue_socket_impl::
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
kqueue_socket_impl::
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
kqueue_socket_impl::
cancel() noexcept
{
    std::shared_ptr<kqueue_socket_impl> self;
    try {
        self = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return;
    }

    conn_.request_cancel();
    rd_.request_cancel();
    wr_.request_cancel();

    kqueue_op* conn_claimed = nullptr;
    kqueue_op* rd_claimed = nullptr;
    kqueue_op* wr_claimed = nullptr;
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
kqueue_socket_impl::
cancel_single_op(kqueue_op& op) noexcept
{
    op.request_cancel();

    kqueue_op** desc_op_ptr = nullptr;
    if (&op == &conn_) desc_op_ptr = &desc_state_.connect_op;
    else if (&op == &rd_) desc_op_ptr = &desc_state_.read_op;
    else if (&op == &wr_) desc_op_ptr = &desc_state_.write_op;

    if (desc_op_ptr)
    {
        kqueue_op* claimed = nullptr;
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
kqueue_socket_impl::
close_socket() noexcept
{
    cancel();

    // Keep impl alive if descriptor_state is queued in the scheduler.
    if (desc_state_.is_enqueued_.load(std::memory_order_acquire))
    {
        try {
            desc_state_.impl_ref_ = shared_from_this();
        } catch (std::bad_weak_ptr const&) {}
    }

    if (fd_ >= 0)
    {
        // Send FIN so the peer gets a reliable kqueue notification
        // before we deregister and close the descriptor.
        ::shutdown(fd_, SHUT_WR);

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

kqueue_socket_service::
kqueue_socket_service(capy::execution_context& ctx)
    : state_(std::make_unique<kqueue_socket_state>(ctx.use_service<kqueue_scheduler>()))
{
}

kqueue_socket_service::
~kqueue_socket_service()
{
}

void
kqueue_socket_service::
shutdown()
{
    std::lock_guard lock(state_->mutex_);

    while (auto* impl = state_->socket_list_.pop_front())
        impl->close_socket();

    // Don't clear socket_ptrs_ here. The scheduler shuts down after us and
    // drains completed_ops_, calling destroy() on each queued op. If we
    // released our shared_ptrs now, a kqueue_op::destroy() could free the
    // last ref to an impl whose embedded descriptor_state is still linked
    // in the queue — use-after-free on the next pop(). Letting ~state_
    // release the ptrs (during service destruction, after scheduler
    // shutdown) keeps every impl alive until all ops have been drained.
}

tcp_socket::socket_impl&
kqueue_socket_service::
create_impl()
{
    auto impl = std::make_shared<kqueue_socket_impl>(*this);
    auto* raw = impl.get();

    {
        std::lock_guard lock(state_->mutex_);
        state_->socket_list_.push_back(raw);
        state_->socket_ptrs_.emplace(raw, std::move(impl));
    }

    return *raw;
}

void
kqueue_socket_service::
destroy_impl(tcp_socket::socket_impl& impl)
{
    auto* kq_impl = static_cast<kqueue_socket_impl*>(&impl);
    std::lock_guard lock(state_->mutex_);
    state_->socket_list_.remove(kq_impl);
    state_->socket_ptrs_.erase(kq_impl);
}

std::error_code
kqueue_socket_service::
open_socket(tcp_socket::socket_impl& impl)
{
    auto* kq_impl = static_cast<kqueue_socket_impl*>(&impl);
    kq_impl->close_socket();

    // FreeBSD: Can use socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return make_err(errno);

    // Set non-blocking
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

    // Set close-on-exec
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }

    // Suppress SIGPIPE on this socket; writev() has no MSG_NOSIGNAL
    // equivalent, so SO_NOSIGPIPE is required on macOS/FreeBSD.
    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }

    kq_impl->fd_ = fd;

    // Register fd with kqueue (edge-triggered mode via EV_CLEAR)
    kq_impl->desc_state_.fd = fd;
    {
        std::lock_guard lock(kq_impl->desc_state_.mutex);
        kq_impl->desc_state_.read_op = nullptr;
        kq_impl->desc_state_.write_op = nullptr;
        kq_impl->desc_state_.connect_op = nullptr;
    }
    scheduler().register_descriptor(fd, &kq_impl->desc_state_);

    return {};
}

void
kqueue_socket_service::
post(kqueue_op* op)
{
    state_->sched_.post(op);
}

void
kqueue_socket_service::
work_started() noexcept
{
    state_->sched_.work_started();
}

void
kqueue_socket_service::
work_finished() noexcept
{
    state_->sched_.work_finished();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE
