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
#include <boost/corosio/native/detail/reactor/reactor_service_state.hpp>

#include <boost/corosio/native/detail/reactor/reactor_op_complete.hpp>

#include <coroutine>
#include <memory>
#include <mutex>
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
    shared_ptr maps (impl_ptrs_) keyed by raw pointer for O(1) lookup and
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

/// State for kqueue socket service.
using kqueue_socket_state =
    reactor_service_state<kqueue_scheduler, kqueue_socket>;

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
    void post(scheduler_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

private:
    std::unique_ptr<kqueue_socket_state> state_;
};

// -- Implementation ---------------------------------------------------------

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
    complete_io_op(*this);
}

inline void
kqueue_connect_op::operator()()
{
    complete_connect_op(*this);
}

inline kqueue_socket::kqueue_socket(kqueue_socket_service& svc) noexcept
    : reactor_socket(svc)
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
    return do_connect(h, ex, ep, token, ec);
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
    return do_read_some(h, ex, param, token, ec, bytes_out);
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
    return do_write_some(h, ex, param, token, ec, bytes_out);
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

inline void
kqueue_socket::cancel() noexcept
{
    do_cancel();
}

inline void
kqueue_socket::close_socket() noexcept
{
    do_close_socket();
    user_set_linger_ = false;
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

    while (auto* impl = state_->impl_list_.pop_front())
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

    // Don't clear impl_ptrs_ here. The scheduler shuts down after us and
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
        state_->impl_ptrs_.emplace(raw, std::move(impl));
        state_->impl_list_.push_back(raw);
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
    state_->impl_list_.remove(kq_impl);
    state_->impl_ptrs_.erase(kq_impl);
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
kqueue_socket_service::post(scheduler_op* op)
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
