//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_SOCKET_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_SOCKET_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/detail/socket_service.hpp>

#include <boost/corosio/native/detail/epoll/epoll_socket.hpp>
#include <boost/corosio/native/detail/epoll/epoll_scheduler.hpp>

#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/capy/buffers.hpp>

#include <coroutine>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
    epoll_socket_service owns all socket impls. destroy_impl() removes the
    shared_ptr from the map, but the impl may survive if ops still hold
    impl_ptr refs. shutdown() closes all sockets and clears the map; any
    in-flight ops will complete and release their refs.
*/

namespace boost::corosio::detail {

/** State for epoll socket service. */
class epoll_socket_state
{
public:
    explicit epoll_socket_state(epoll_scheduler& sched) noexcept : sched_(sched)
    {
    }

    epoll_scheduler& sched_;
    std::mutex mutex_;
    intrusive_list<epoll_socket> socket_list_;
    std::unordered_map<epoll_socket*, std::shared_ptr<epoll_socket>>
        socket_ptrs_;
};

/** epoll socket service implementation.

    Inherits from socket_service to enable runtime polymorphism.
    Uses key_type = socket_service for service lookup.
*/
class BOOST_COROSIO_DECL epoll_socket_service final : public socket_service
{
public:
    explicit epoll_socket_service(capy::execution_context& ctx);
    ~epoll_socket_service() override;

    epoll_socket_service(epoll_socket_service const&)            = delete;
    epoll_socket_service& operator=(epoll_socket_service const&) = delete;

    void shutdown() override;

    io_object::implementation* construct() override;
    void destroy(io_object::implementation*) override;
    void close(io_object::handle&) override;
    std::error_code open_socket(
        tcp_socket::implementation& impl,
        int family,
        int type,
        int protocol) override;

    epoll_scheduler& scheduler() const noexcept
    {
        return state_->sched_;
    }
    void post(epoll_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

private:
    std::unique_ptr<epoll_socket_state> state_;
};

//--------------------------------------------------------------------------
//
// Implementation
//
//--------------------------------------------------------------------------

// Register an op with the reactor, handling cached edge events.
// Called under the EAGAIN/EINPROGRESS path when speculative I/O failed.
inline void
epoll_socket::register_op(
    epoll_op& op,
    epoll_op*& desc_slot,
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

inline void
epoll_op::canceller::operator()() const noexcept
{
    op->cancel();
}

inline void
epoll_connect_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
epoll_read_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
epoll_write_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
epoll_op::operator()()
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
    capy::executor_ref saved_ex(ex);
    std::coroutine_handle<> saved_h(h);
    auto prevent_premature_destruction = std::move(impl_ptr);
    dispatch_coro(saved_ex, saved_h).resume();
}

inline void
epoll_connect_op::operator()()
{
    stop_cb.reset();

    socket_impl_->svc_.scheduler().reset_inline_budget();

    bool success = (errn == 0 && !cancelled.load(std::memory_order_acquire));

    // Cache endpoints on successful connect
    if (success && socket_impl_)
    {
        endpoint local_ep;
        sockaddr_storage local_storage{};
        socklen_t local_len = sizeof(local_storage);
        if (::getsockname(
                fd, reinterpret_cast<sockaddr*>(&local_storage), &local_len) ==
            0)
            local_ep = from_sockaddr(local_storage);
        static_cast<epoll_socket*>(socket_impl_)
            ->set_endpoints(local_ep, target_endpoint);
    }

    if (cancelled.load(std::memory_order_acquire))
        *ec_out = capy::error::canceled;
    else if (errn != 0)
        *ec_out = make_err(errn);
    else
        *ec_out = {};

    // Move to stack before resuming. See epoll_op::operator()() for rationale.
    capy::executor_ref saved_ex(ex);
    std::coroutine_handle<> saved_h(h);
    auto prevent_premature_destruction = std::move(impl_ptr);
    dispatch_coro(saved_ex, saved_h).resume();
}

inline epoll_socket::epoll_socket(epoll_socket_service& svc) noexcept
    : svc_(svc)
{
}

inline epoll_socket::~epoll_socket() = default;

inline std::coroutine_handle<>
epoll_socket::connect(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    endpoint ep,
    std::stop_token token,
    std::error_code* ec)
{
    auto& op = conn_;

    sockaddr_storage storage{};
    socklen_t addrlen =
        detail::to_sockaddr(ep, detail::socket_family(fd_), storage);
    int result = ::connect(fd_, reinterpret_cast<sockaddr*>(&storage), addrlen);

    if (result == 0)
    {
        sockaddr_storage local_storage{};
        socklen_t local_len = sizeof(local_storage);
        if (::getsockname(
                fd_, reinterpret_cast<sockaddr*>(&local_storage), &local_len) ==
            0)
            local_endpoint_ = detail::from_sockaddr(local_storage);
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
        op.start(token, this);
        op.impl_ptr = shared_from_this();
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
    op.start(token, this);
    op.impl_ptr = shared_from_this();

    register_op(
        op, desc_state_.connect_op, desc_state_.write_ready,
        desc_state_.connect_cancel_pending);
    return std::noop_coroutine();
}

inline std::coroutine_handle<>
epoll_socket::read_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = rd_;
    op.reset();

    capy::mutable_buffer bufs[epoll_read_op::max_buffers];
    op.iovec_count =
        static_cast<int>(param.copy_to(bufs, epoll_read_op::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.empty_buffer_read = true;
        op.h                 = h;
        op.ex                = ex;
        op.ec_out            = ec;
        op.bytes_out         = bytes_out;
        op.start(token, this);
        op.impl_ptr = shared_from_this();
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
        op.start(token, this);
        op.impl_ptr = shared_from_this();
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
    op.start(token, this);
    op.impl_ptr = shared_from_this();

    register_op(
        op, desc_state_.read_op, desc_state_.read_ready,
        desc_state_.read_cancel_pending);
    return std::noop_coroutine();
}

inline std::coroutine_handle<>
epoll_socket::write_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = wr_;
    op.reset();

    capy::mutable_buffer bufs[epoll_write_op::max_buffers];
    op.iovec_count =
        static_cast<int>(param.copy_to(bufs, epoll_write_op::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.h         = h;
        op.ex        = ex;
        op.ec_out    = ec;
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
        op.iovecs[i].iov_len  = bufs[i].size();
    }

    // Speculative write
    msghdr msg{};
    msg.msg_iov    = op.iovecs;
    msg.msg_iovlen = static_cast<std::size_t>(op.iovec_count);

    ssize_t n;
    do
    {
        n = ::sendmsg(fd_, &msg, MSG_NOSIGNAL);
    }
    while (n < 0 && errno == EINTR);

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
        op.start(token, this);
        op.impl_ptr = shared_from_this();
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
    op.start(token, this);
    op.impl_ptr = shared_from_this();

    register_op(
        op, desc_state_.write_op, desc_state_.write_ready,
        desc_state_.write_cancel_pending);
    return std::noop_coroutine();
}

inline std::error_code
epoll_socket::shutdown(tcp_socket::shutdown_type what) noexcept
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
epoll_socket::set_option(
    int level, int optname, void const* data, std::size_t size) noexcept
{
    if (::setsockopt(fd_, level, optname, data, static_cast<socklen_t>(size)) !=
        0)
        return make_err(errno);
    return {};
}

inline std::error_code
epoll_socket::get_option(
    int level, int optname, void* data, std::size_t* size) const noexcept
{
    socklen_t len = static_cast<socklen_t>(*size);
    if (::getsockopt(fd_, level, optname, data, &len) != 0)
        return make_err(errno);
    *size = static_cast<std::size_t>(len);
    return {};
}

inline void
epoll_socket::cancel() noexcept
{
    auto self = weak_from_this().lock();
    if (!self)
        return;

    conn_.request_cancel();
    rd_.request_cancel();
    wr_.request_cancel();

    epoll_op* conn_claimed = nullptr;
    epoll_op* rd_claimed   = nullptr;
    epoll_op* wr_claimed   = nullptr;
    {
        std::lock_guard lock(desc_state_.mutex);
        if (desc_state_.connect_op == &conn_)
            conn_claimed = std::exchange(desc_state_.connect_op, nullptr);
        else
            desc_state_.connect_cancel_pending = true;
        if (desc_state_.read_op == &rd_)
            rd_claimed = std::exchange(desc_state_.read_op, nullptr);
        else
            desc_state_.read_cancel_pending = true;
        if (desc_state_.write_op == &wr_)
            wr_claimed = std::exchange(desc_state_.write_op, nullptr);
        else
            desc_state_.write_cancel_pending = true;
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

inline void
epoll_socket::cancel_single_op(epoll_op& op) noexcept
{
    auto self = weak_from_this().lock();
    if (!self)
        return;

    op.request_cancel();

    epoll_op** desc_op_ptr = nullptr;
    if (&op == &conn_)
        desc_op_ptr = &desc_state_.connect_op;
    else if (&op == &rd_)
        desc_op_ptr = &desc_state_.read_op;
    else if (&op == &wr_)
        desc_op_ptr = &desc_state_.write_op;

    if (desc_op_ptr)
    {
        epoll_op* claimed = nullptr;
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

inline void
epoll_socket::close_socket() noexcept
{
    auto self = weak_from_this().lock();
    if (self)
    {
        conn_.request_cancel();
        rd_.request_cancel();
        wr_.request_cancel();

        epoll_op* conn_claimed = nullptr;
        epoll_op* rd_claimed   = nullptr;
        epoll_op* wr_claimed   = nullptr;
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

        if (desc_state_.is_enqueued_.load(std::memory_order_acquire))
            desc_state_.impl_ref_ = self;
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

inline epoll_socket_service::epoll_socket_service(capy::execution_context& ctx)
    : state_(
          std::make_unique<epoll_socket_state>(
              ctx.use_service<epoll_scheduler>()))
{
}

inline epoll_socket_service::~epoll_socket_service() {}

inline void
epoll_socket_service::shutdown()
{
    std::lock_guard lock(state_->mutex_);

    while (auto* impl = state_->socket_list_.pop_front())
        impl->close_socket();

    // Don't clear socket_ptrs_ here. The scheduler shuts down after us and
    // drains completed_ops_, calling destroy() on each queued op. If we
    // released our shared_ptrs now, an epoll_op::destroy() could free the
    // last ref to an impl whose embedded descriptor_state is still linked
    // in the queue — use-after-free on the next pop(). Letting ~state_
    // release the ptrs (during service destruction, after scheduler
    // shutdown) keeps every impl alive until all ops have been drained.
}

inline io_object::implementation*
epoll_socket_service::construct()
{
    auto impl = std::make_shared<epoll_socket>(*this);
    auto* raw = impl.get();

    {
        std::lock_guard lock(state_->mutex_);
        state_->socket_list_.push_back(raw);
        state_->socket_ptrs_.emplace(raw, std::move(impl));
    }

    return raw;
}

inline void
epoll_socket_service::destroy(io_object::implementation* impl)
{
    auto* epoll_impl = static_cast<epoll_socket*>(impl);
    epoll_impl->close_socket();
    std::lock_guard lock(state_->mutex_);
    state_->socket_list_.remove(epoll_impl);
    state_->socket_ptrs_.erase(epoll_impl);
}

inline std::error_code
epoll_socket_service::open_socket(
    tcp_socket::implementation& impl, int family, int type, int protocol)
{
    auto* epoll_impl = static_cast<epoll_socket*>(&impl);
    epoll_impl->close_socket();

    int fd = ::socket(family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
    if (fd < 0)
        return make_err(errno);

    if (family == AF_INET6)
    {
        int one = 1;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    }

    epoll_impl->fd_ = fd;

    // Register fd with epoll (edge-triggered mode)
    epoll_impl->desc_state_.fd = fd;
    {
        std::lock_guard lock(epoll_impl->desc_state_.mutex);
        epoll_impl->desc_state_.read_op    = nullptr;
        epoll_impl->desc_state_.write_op   = nullptr;
        epoll_impl->desc_state_.connect_op = nullptr;
    }
    scheduler().register_descriptor(fd, &epoll_impl->desc_state_);

    return {};
}

inline void
epoll_socket_service::close(io_object::handle& h)
{
    static_cast<epoll_socket*>(h.get())->close_socket();
}

inline void
epoll_socket_service::post(epoll_op* op)
{
    state_->sched_.post(op);
}

inline void
epoll_socket_service::work_started() noexcept
{
    state_->sched_.work_started();
}

inline void
epoll_socket_service::work_finished() noexcept
{
    state_->sched_.work_finished();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_SOCKET_SERVICE_HPP
