//
// Copyright (c) 2026 Michael Vandeberg
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_SOCKET_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_SOCKET_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/detail/socket_service.hpp>

#include <boost/corosio/native/detail/kqueue/kqueue_socket.hpp>
#include <boost/corosio/native/detail/kqueue/kqueue_scheduler.hpp>

#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/capy/buffers.hpp>

#include <coroutine>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

/*
    kqueue Socket Implementation
    ============================

    Each I/O operation follows the same pattern:
      1. Try the syscall speculatively (readv/writev) before suspending
      2. On success, return via symmetric transfer (the "pump" fast path)
      3. On budget exhaustion, post to the scheduler queue for fairness
      4. On EAGAIN, register_op() parks the op in the descriptor_state

    The speculative path avoids scheduler queue, mutex, and reactor
    round-trips entirely. An inline budget limits consecutive inline
    completions to prevent starvation of other connections.

    Cancellation
    ------------
    See op.hpp for the completion/cancellation race handling via the
    descriptor_state mutex. cancel() must complete pending operations (post
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
    kqueue_socket_service owns all socket impls. destroy_impl() removes the
    shared_ptr from the map, but the impl may survive if ops still hold
    impl_ptr refs. shutdown() closes all sockets and clears the map; any
    in-flight ops will complete and release their refs.
*/

/*
    kqueue socket implementation
    ============================

    Each kqueue_socket owns a descriptor_state that is persistently
    registered with kqueue (EVFILT_READ + EVFILT_WRITE, both EV_CLEAR for
    edge-triggered semantics). The descriptor_state tracks three operation
    slots (read_op, write_op, connect_op) and two ready flags
    (read_ready, write_ready) under a per-descriptor mutex.

    Speculative I/O and the pump
    ----------------------------
    read_some() and write_some() attempt the syscall (readv/writev)
    speculatively before suspending the caller. If data is available the
    result is returned via symmetric transfer — no scheduler queue, no
    mutex, no reactor round-trip. An inline budget limits consecutive
    inline completions to prevent starvation of other connections.

    When the speculative attempt returns EAGAIN, register_op() parks the
    operation in its descriptor_state slot under the per-descriptor mutex.
    If a cached ready flag fires before parking, register_op() retries
    the I/O once under the mutex. This eliminates the cached_initiator
    coroutine frame that previously trampolined into do_read_io() /
    do_write_io() after the caller suspended.

    Ready-flag protocol
    -------------------
    When a kqueue event fires and no operation is pending for that
    direction, the reactor sets the corresponding ready flag instead of
    dropping the event. When register_op() finds the ready flag set, it
    performs I/O immediately rather than parking. This prevents lost
    wakeups under edge-triggered notification.
*/

namespace boost::corosio::detail {

/** State for kqueue socket service. */
class kqueue_socket_state
{
public:
    explicit kqueue_socket_state(kqueue_scheduler& sched) noexcept
        : sched_(sched)
    {
    }

    kqueue_scheduler& sched_;
    std::mutex mutex_;
    intrusive_list<kqueue_socket> socket_list_;
    std::unordered_map<kqueue_socket*, std::shared_ptr<kqueue_socket>>
        socket_ptrs_;
};

/** kqueue socket service implementation.

    Inherits from socket_service to enable runtime polymorphism.
    Uses key_type = socket_service for service lookup.
*/
class BOOST_COROSIO_DECL kqueue_socket_service final : public socket_service
{
public:
    explicit kqueue_socket_service(capy::execution_context& ctx);
    ~kqueue_socket_service();

    kqueue_socket_service(kqueue_socket_service const&)            = delete;
    kqueue_socket_service& operator=(kqueue_socket_service const&) = delete;

    void shutdown() override;

    io_object::implementation* construct() override;
    void destroy(io_object::implementation*) override;
    void close(io_object::handle&) override;
    std::error_code open_socket(
        tcp_socket::implementation& impl,
        int family,
        int type,
        int protocol) override;

    kqueue_scheduler& scheduler() const noexcept
    {
        return state_->sched_;
    }
    void post(kqueue_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

private:
    std::unique_ptr<kqueue_socket_state> state_;
};

// -- Implementation ---------------------------------------------------------

inline void
kqueue_op::canceller::operator()() const noexcept
{
    op->cancel();
}

inline void
kqueue_connect_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
kqueue_read_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
kqueue_write_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
kqueue_op::operator()()
{
    stop_cb.reset();

    socket_impl_->desc_state_.scheduler_->reset_inline_budget();

    if (ec_out)
    {
        if (cancelled.load(std::memory_order_acquire))
            *ec_out = capy::error::canceled;
        else if (errn != 0)
            *ec_out = make_err(errn);
        else if (is_read_operation() && bytes_transferred == 0)
            *ec_out = capy::error::eof;
        else
            *ec_out = {};
    }

    if (bytes_out)
        *bytes_out = bytes_transferred;

    // Move to stack before resuming coroutine. The coroutine might close
    // the socket, releasing the last wrapper ref. If impl_ptr were the
    // last ref and we destroyed it while still in operator(), we'd have
    // use-after-free. Moving to local ensures destruction happens at
    // function exit, after all member accesses are complete.
    capy::executor_ref saved_ex(std::move(ex));
    std::coroutine_handle<> saved_h(std::move(h));
    auto prevent_premature_destruction = std::move(impl_ptr);
    dispatch_coro(saved_ex, saved_h).resume();
}

inline void
kqueue_connect_op::operator()()
{
    stop_cb.reset();

    socket_impl_->desc_state_.scheduler_->reset_inline_budget();

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
        static_cast<kqueue_socket*>(socket_impl_)
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

    // Move to stack before resuming. See kqueue_op::operator()() for rationale.
    capy::executor_ref saved_ex(std::move(ex));
    std::coroutine_handle<> saved_h(std::move(h));
    auto prevent_premature_destruction = std::move(impl_ptr);
    dispatch_coro(saved_ex, saved_h).resume();
}

inline kqueue_socket::kqueue_socket(kqueue_socket_service& svc) noexcept
    : svc_(svc)
{
}

inline kqueue_socket::~kqueue_socket() = default;

inline std::coroutine_handle<>
kqueue_socket::connect(
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

    // Cache endpoints on sync success
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

        // Budget exhausted — post through queue
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

    // EINPROGRESS — async path
    svc_.scheduler().ensure_write_filter(fd_, &desc_state_);

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

// Register an op with the reactor, handling cached edge events.
// Called under the EAGAIN path when speculative I/O failed.
inline void
kqueue_socket::register_op(
    kqueue_op& op,
    kqueue_op*& desc_slot,
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

inline std::coroutine_handle<>
kqueue_socket::read_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = rd_;
    op.reset();

    capy::mutable_buffer bufs[kqueue_read_op::max_buffers];
    op.iovec_count =
        static_cast<int>(param.copy_to(bufs, kqueue_read_op::max_buffers));

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

    // Speculative read: try I/O before suspending. On success, return via
    // symmetric transfer without touching the scheduler queue — this creates
    // a tight pump loop for back-to-back reads on a hot socket.
    // Budget limits consecutive inline completions to prevent starvation
    // of other connections competing for scheduler time.
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

        // Budget exhausted — fall through to queue
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
kqueue_socket::write_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = wr_;
    op.reset();

    capy::mutable_buffer bufs[kqueue_write_op::max_buffers];
    op.iovec_count =
        static_cast<int>(param.copy_to(bufs, kqueue_write_op::max_buffers));

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

    // Speculative write: try I/O before suspending. On success, return via
    // symmetric transfer without touching the scheduler queue — this creates
    // a tight pump loop for back-to-back writes on a hot socket.
    // Budget limits consecutive inline completions to prevent starvation.
    ssize_t n;
    do
    {
        n = ::writev(fd_, op.iovecs, op.iovec_count);
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

        // Budget exhausted — fall through to queue
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
    svc_.scheduler().ensure_write_filter(fd_, &desc_state_);

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
kqueue_socket::shutdown(tcp_socket::shutdown_type what) noexcept
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
kqueue_socket::set_option(
    int level, int optname, void const* data, std::size_t size) noexcept
{
    if (::setsockopt(fd_, level, optname, data, static_cast<socklen_t>(size)) !=
        0)
        return make_err(errno);
    if (level == SOL_SOCKET && optname == SO_LINGER &&
        size >= sizeof(struct ::linger))
        user_set_linger_ =
            static_cast<struct ::linger const*>(data)->l_onoff != 0;
    return {};
}

inline std::error_code
kqueue_socket::get_option(
    int level, int optname, void* data, std::size_t* size) const noexcept
{
    socklen_t len = static_cast<socklen_t>(*size);
    if (::getsockopt(fd_, level, optname, data, &len) != 0)
        return make_err(errno);
    *size = static_cast<std::size_t>(len);
    return {};
}

inline void
kqueue_socket::cancel() noexcept
{
    auto self = weak_from_this().lock();
    if (!self)
        return;

    conn_.request_cancel();
    rd_.request_cancel();
    wr_.request_cancel();

    kqueue_op* conn_claimed = nullptr;
    kqueue_op* rd_claimed   = nullptr;
    kqueue_op* wr_claimed   = nullptr;
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
kqueue_socket::cancel_single_op(kqueue_op& op) noexcept
{
    auto self = weak_from_this().lock();
    if (!self)
        return;

    op.request_cancel();

    kqueue_op** desc_op_ptr = nullptr;
    if (&op == &conn_)
        desc_op_ptr = &desc_state_.connect_op;
    else if (&op == &rd_)
        desc_op_ptr = &desc_state_.read_op;
    else if (&op == &wr_)
        desc_op_ptr = &desc_state_.write_op;

    if (desc_op_ptr)
    {
        kqueue_op* claimed = nullptr;
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
kqueue_socket::close_socket() noexcept
{
    auto self = weak_from_this().lock();
    if (self)
    {
        conn_.request_cancel();
        rd_.request_cancel();
        wr_.request_cancel();

        kqueue_op* conn_claimed = nullptr;
        kqueue_op* rd_claimed   = nullptr;
        kqueue_op* wr_claimed   = nullptr;
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
        ::close(fd_);
        fd_ = -1;
    }

    desc_state_.fd                = -1;
    desc_state_.registered_events = 0;
    user_set_linger_              = false;

    local_endpoint_  = endpoint{};
    remote_endpoint_ = endpoint{};
}

inline kqueue_socket_service::kqueue_socket_service(
    capy::execution_context& ctx)
    : state_(
          std::make_unique<kqueue_socket_state>(
              ctx.use_service<kqueue_scheduler>()))
{
}

inline kqueue_socket_service::~kqueue_socket_service() {}

inline void
kqueue_socket_service::shutdown()
{
    std::lock_guard lock(state_->mutex_);

    while (auto* impl = state_->socket_list_.pop_front())
    {
        if (impl->user_set_linger_ && impl->fd_ >= 0)
        {
            struct ::linger lg;
            lg.l_onoff  = 0;
            lg.l_linger = 0;
            ::setsockopt(impl->fd_, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        }
        impl->close_socket();
    }

    // Don't clear socket_ptrs_ here. The scheduler shuts down after us and
    // drains completed_ops_, calling destroy() on each queued op. If we
    // released our shared_ptrs now, a kqueue_op::destroy() could free the
    // last ref to an impl whose embedded descriptor_state is still linked
    // in the queue — use-after-free on the next pop(). Letting ~state_
    // release the ptrs (during service destruction, after scheduler
    // shutdown) keeps every impl alive until all ops have been drained.
}

inline io_object::implementation*
kqueue_socket_service::construct()
{
    auto impl = std::make_shared<kqueue_socket>(*this);
    auto* raw = impl.get();

    {
        std::lock_guard lock(state_->mutex_);
        state_->socket_list_.push_back(raw);
        state_->socket_ptrs_.emplace(raw, std::move(impl));
    }

    return raw;
}

inline void
kqueue_socket_service::destroy(io_object::implementation* impl)
{
    auto* kq_impl = static_cast<kqueue_socket*>(impl);

    // Match asio: if the user set SO_LINGER, clear it before close so
    // the destructor doesn't block and close() sends FIN instead of RST.
    // RST doesn't reliably trigger EV_EOF on macOS kqueue.
    if (kq_impl->user_set_linger_ && kq_impl->fd_ >= 0)
    {
        struct ::linger lg;
        lg.l_onoff  = 0;
        lg.l_linger = 0;
        ::setsockopt(kq_impl->fd_, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    }

    kq_impl->close_socket();
    std::lock_guard lock(state_->mutex_);
    state_->socket_list_.remove(kq_impl);
    state_->socket_ptrs_.erase(kq_impl);
}

inline std::error_code
kqueue_socket_service::open_socket(
    tcp_socket::implementation& impl, int family, int type, int protocol)
{
    auto* kq_impl = static_cast<kqueue_socket*>(&impl);
    kq_impl->close_socket();

    int fd = ::socket(family, type, protocol);
    if (fd < 0)
        return make_err(errno);

    if (family == AF_INET6)
    {
        int v6only = 1;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    }

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
        kq_impl->desc_state_.read_op    = nullptr;
        kq_impl->desc_state_.write_op   = nullptr;
        kq_impl->desc_state_.connect_op = nullptr;
    }
    scheduler().register_descriptor(fd, &kq_impl->desc_state_);

    return {};
}

inline void
kqueue_socket_service::close(io_object::handle& h)
{
    static_cast<kqueue_socket*>(h.get())->close_socket();
}

inline void
kqueue_socket_service::post(kqueue_op* op)
{
    state_->sched_.post(op);
}

inline void
kqueue_socket_service::work_started() noexcept
{
    state_->sched_.work_started();
}

inline void
kqueue_socket_service::work_finished() noexcept
{
    state_->sched_.work_finished();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_SOCKET_SERVICE_HPP
