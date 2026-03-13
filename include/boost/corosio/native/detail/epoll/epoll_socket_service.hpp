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
#include <boost/corosio/native/detail/reactor/reactor_service_state.hpp>

#include <boost/corosio/native/detail/reactor/reactor_op_complete.hpp>

#include <coroutine>
#include <mutex>
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
    epoll_socket_service owns all socket impls. destroy_impl() removes the
    shared_ptr from the map, but the impl may survive if ops still hold
    impl_ptr refs. shutdown() closes all sockets and clears the map; any
    in-flight ops will complete and release their refs.
*/

namespace boost::corosio::detail {

/// State for epoll socket service.
using epoll_socket_state = reactor_service_state<epoll_scheduler, epoll_socket>;

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
    void post(scheduler_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

private:
    std::unique_ptr<epoll_socket_state> state_;
};

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
    complete_io_op(*this);
}

inline void
epoll_connect_op::operator()()
{
    complete_connect_op(*this);
}

inline epoll_socket::epoll_socket(epoll_socket_service& svc) noexcept
    : reactor_socket(svc)
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
    return do_connect(h, ex, ep, token, ec);
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
    return do_read_some(h, ex, param, token, ec, bytes_out);
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
    return do_write_some(h, ex, param, token, ec, bytes_out);
}

inline void
epoll_socket::cancel() noexcept
{
    do_cancel();
}

inline void
epoll_socket::close_socket() noexcept
{
    do_close_socket();
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

    while (auto* impl = state_->impl_list_.pop_front())
        impl->close_socket();

    // Don't clear impl_ptrs_ here. The scheduler shuts down after us and
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
        state_->impl_ptrs_.emplace(raw, std::move(impl));
        state_->impl_list_.push_back(raw);
    }

    return raw;
}

inline void
epoll_socket_service::destroy(io_object::implementation* impl)
{
    auto* epoll_impl = static_cast<epoll_socket*>(impl);
    epoll_impl->close_socket();
    std::lock_guard lock(state_->mutex_);
    state_->impl_list_.remove(epoll_impl);
    state_->impl_ptrs_.erase(epoll_impl);
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
epoll_socket_service::post(scheduler_op* op)
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
