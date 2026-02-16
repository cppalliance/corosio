//
// Copyright (c) 2026 Michael Vandeberg
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include "src/detail/kqueue/acceptors.hpp"
#include "src/detail/kqueue/sockets.hpp"
#include "src/detail/endpoint_convert.hpp"
#include "src/detail/make_err.hpp"
#include "src/detail/dispatch_coro.hpp"

#include <utility>

/*
    kqueue async accept implementation
    ===================================

    kqueue_acceptor_impl registers its listening fd with kqueue once
    (EVFILT_READ, EV_CLEAR for edge-triggered semantics) via
    desc_state_. A single accept operation can be pending at a time,
    stored in desc_state_.read_op since accept is a read-like event.

    Async accept control flow
    -------------------------
    accept() first attempts a synchronous ::accept(). On EAGAIN the
    ready flag is checked under the desc_state_ mutex: if set, a retry
    loop calls perform_io() until the accept succeeds or the flag is
    exhausted. Otherwise the op is parked in desc_state_.read_op for
    the reactor to wake later. After parking, a cancellation race-check
    reclaims the op if a stop was requested between parking and the
    check.

    Completion and coroutine resumption
    ------------------------------------
    kqueue_accept_op::operator()() runs on the scheduler thread. On
    success it creates a kqueue_socket_impl for the accepted fd,
    registers it with kqueue, sets SO_NOSIGPIPE, and caches both
    endpoints. The coroutine is resumed via saved_ex.dispatch() after
    all member state has been moved to stack locals.

    Lifetime management
    -------------------
    shared_from_this() is captured in op.impl_ptr whenever an op is
    posted to the scheduler. This shared_ptr prevents the acceptor
    impl from being destroyed while completions are in flight. The
    desc_state_.impl_ref_ similarly prevents destruction while the
    descriptor_state itself is enqueued in the scheduler's ready queue.

    Cancellation
    ------------
    cancel() and cancel_single_op() set the cancelled flag, then claim
    the op from desc_state_.read_op under the mutex. If claimed, the
    op is posted for completion with a cancelled error code and the
    extra work_started() from registration is balanced by work_finished().
*/

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

void
kqueue_accept_op::cancel() noexcept
{
    if (acceptor_impl_)
        acceptor_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

void
kqueue_accept_op::operator()()
{
    stop_cb.reset();

    static_cast<kqueue_acceptor_impl*>(acceptor_impl_)
        ->service()
        .scheduler()
        .reset_inline_budget();

    bool success = (errn == 0 && !cancelled.load(std::memory_order_acquire));

    if (ec_out)
    {
        if (cancelled.load(std::memory_order_acquire))
            *ec_out = capy::error::canceled;
        else if (errn != 0)
            *ec_out = make_err(errn);
        else
            *ec_out = {};
    }

    if (success && accepted_fd >= 0)
    {
        if (acceptor_impl_)
        {
            auto* socket_svc =
                static_cast<kqueue_acceptor_impl*>(acceptor_impl_)
                    ->service()
                    .socket_service();
            if (socket_svc)
            {
                auto& impl =
                    static_cast<kqueue_socket_impl&>(*socket_svc->construct());
                impl.set_socket(accepted_fd);

                // Register accepted socket with kqueue (edge-triggered via EV_CLEAR)
                impl.desc_state_.fd = accepted_fd;
                {
                    std::lock_guard lock(impl.desc_state_.mutex);
                    impl.desc_state_.read_op = nullptr;
                    impl.desc_state_.write_op = nullptr;
                    impl.desc_state_.connect_op = nullptr;
                }
                socket_svc->scheduler().register_descriptor(
                    accepted_fd, &impl.desc_state_);

                // Suppress SIGPIPE on the accepted socket; macOS lacks MSG_NOSIGNAL
                int one = 1;
                if (::setsockopt(
                        accepted_fd, SOL_SOCKET, SO_NOSIGPIPE, &one,
                        sizeof(one)) == -1)
                {
                    if (ec_out)
                        *ec_out = make_err(errno);
                    ::close(accepted_fd);
                    accepted_fd = -1;
                    socket_svc->destroy(&impl);
                    if (impl_out)
                        *impl_out = nullptr;
                }
                else
                {
                    sockaddr_in local_addr{};
                    socklen_t local_len = sizeof(local_addr);
                    sockaddr_in remote_addr{};
                    socklen_t remote_len = sizeof(remote_addr);

                    endpoint local_ep, remote_ep;
                    if (::getsockname(
                            accepted_fd,
                            reinterpret_cast<sockaddr*>(&local_addr),
                            &local_len) == 0)
                        local_ep = from_sockaddr_in(local_addr);
                    if (::getpeername(
                            accepted_fd,
                            reinterpret_cast<sockaddr*>(&remote_addr),
                            &remote_len) == 0)
                        remote_ep = from_sockaddr_in(remote_addr);

                    impl.set_endpoints(local_ep, remote_ep);

                    if (impl_out)
                        *impl_out = &impl;

                    accepted_fd = -1;
                }
            }
            else
            {
                // Socket service not registered in execution_context
                if (ec_out && !*ec_out)
                    *ec_out = make_err(ENOENT);
                ::close(accepted_fd);
                accepted_fd = -1;
                if (impl_out)
                    *impl_out = nullptr;
            }
        }
        else
        {
            ::close(accepted_fd);
            accepted_fd = -1;
            if (impl_out)
                *impl_out = nullptr;
        }
    }
    else
    {
        if (accepted_fd >= 0)
        {
            ::close(accepted_fd);
            accepted_fd = -1;
        }

        if (peer_impl)
        {
            auto* socket_svc_cleanup =
                static_cast<kqueue_acceptor_impl*>(acceptor_impl_)
                    ->service()
                    .socket_service();
            if (socket_svc_cleanup)
                socket_svc_cleanup->destroy(peer_impl);
            peer_impl = nullptr;
        }

        if (impl_out)
            *impl_out = nullptr;
    }

    // Move to stack before resuming. See kqueue_op::operator()() for rationale.
    capy::executor_ref saved_ex(std::move(ex));
    std::coroutine_handle<> saved_h(std::move(h));
    auto prevent_premature_destruction = std::move(impl_ptr);
    dispatch_coro(saved_ex, saved_h).resume();
}

kqueue_acceptor_impl::kqueue_acceptor_impl(
    kqueue_acceptor_service& svc) noexcept
    : svc_(svc)
{
}

std::coroutine_handle<>
kqueue_acceptor_impl::accept(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    std::stop_token token,
    std::error_code* ec,
    io_object::implementation** impl_out)
{
    auto& op = acc_;
    op.reset();
    op.h = h;
    op.ex = ex;
    op.ec_out = ec;
    op.impl_out = impl_out;
    op.fd = fd_;
    op.start(token, this);

    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);

    // FreeBSD: Can use accept4(fd_, addr, addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC)
    int accepted = ::accept(fd_, reinterpret_cast<sockaddr*>(&addr), &addrlen);

    if (accepted >= 0)
    {
        // Set non-blocking and close-on-exec on the accepted socket
        int flags = ::fcntl(accepted, F_GETFL, 0);
        if (flags == -1 || ::fcntl(accepted, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            int errn = errno;
            ::close(accepted);
            op.complete(errn, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return std::noop_coroutine();
        }
        if (::fcntl(accepted, F_SETFD, FD_CLOEXEC) == -1)
        {
            int errn = errno;
            ::close(accepted);
            op.complete(errn, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return std::noop_coroutine();
        }

        {
            std::lock_guard lock(desc_state_.mutex);
            desc_state_.read_ready = false;
        }
        op.accepted_fd = accepted;
        op.complete(0, 0);
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return std::noop_coroutine();
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        svc_.work_started();
        op.impl_ptr = shared_from_this();

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
                    break;
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
            return std::noop_coroutine();
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
        return std::noop_coroutine();
    }

    op.complete(errno, 0);
    op.impl_ptr = shared_from_this();
    svc_.post(&op);
    return std::noop_coroutine();
}

void
kqueue_acceptor_impl::cancel() noexcept
{
    auto self = weak_from_this().lock();
    if (!self)
        return;

    acc_.request_cancel();

    kqueue_op* claimed = nullptr;
    {
        std::lock_guard lock(desc_state_.mutex);
        if (desc_state_.read_op == &acc_)
            claimed = std::exchange(desc_state_.read_op, nullptr);
    }
    if (claimed)
    {
        acc_.impl_ptr = self;
        svc_.post(&acc_);
        svc_.work_finished();
    }
}

void
kqueue_acceptor_impl::cancel_single_op(kqueue_op& op) noexcept
{
    auto self = weak_from_this().lock();
    if (!self)
        return;

    op.request_cancel();

    kqueue_op* claimed = nullptr;
    {
        std::lock_guard lock(desc_state_.mutex);
        if (desc_state_.read_op == &op)
            claimed = std::exchange(desc_state_.read_op, nullptr);
    }
    if (claimed)
    {
        op.impl_ptr = self;
        svc_.post(&op);
        svc_.work_finished();
    }
}

void
kqueue_acceptor_impl::close_socket() noexcept
{
    auto self = weak_from_this().lock();
    if (self)
    {
        acc_.request_cancel();

        kqueue_op* claimed = nullptr;
        {
            std::lock_guard lock(desc_state_.mutex);
            claimed = std::exchange(desc_state_.read_op, nullptr);
            desc_state_.read_ready = false;
            desc_state_.write_ready = false;
        }

        if (claimed)
        {
            acc_.impl_ptr = self;
            svc_.post(&acc_);
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

    desc_state_.fd = -1;
    desc_state_.registered_events = 0;

    local_endpoint_ = endpoint{};
}

kqueue_acceptor_service::kqueue_acceptor_service(capy::execution_context& ctx)
    : ctx_(ctx)
    , state_(
          std::make_unique<kqueue_acceptor_state>(
              ctx.use_service<kqueue_scheduler>()))
{
}

kqueue_acceptor_service::~kqueue_acceptor_service() = default;

void
kqueue_acceptor_service::shutdown()
{
    std::lock_guard lock(state_->mutex_);

    while (auto* impl = state_->acceptor_list_.pop_front())
        impl->close_socket();

    // Don't clear acceptor_ptrs_ here — same rationale as
    // kqueue_socket_service::shutdown(). Let ~state_ release ptrs
    // after scheduler shutdown has drained all queued ops.
}

io_object::implementation*
kqueue_acceptor_service::construct()
{
    auto impl = std::make_shared<kqueue_acceptor_impl>(*this);
    auto* raw = impl.get();

    std::lock_guard lock(state_->mutex_);
    state_->acceptor_list_.push_back(raw);
    state_->acceptor_ptrs_.emplace(raw, std::move(impl));

    return raw;
}

void
kqueue_acceptor_service::destroy(io_object::implementation* impl)
{
    auto* kq_impl = static_cast<kqueue_acceptor_impl*>(impl);
    kq_impl->close_socket();
    std::lock_guard lock(state_->mutex_);
    state_->acceptor_list_.remove(kq_impl);
    state_->acceptor_ptrs_.erase(kq_impl);
}

void
kqueue_acceptor_service::close(io_object::handle& h)
{
    static_cast<kqueue_acceptor_impl*>(h.get())->close_socket();
}

std::error_code
kqueue_acceptor_service::open_acceptor(
    tcp_acceptor::implementation& impl, endpoint ep, int backlog)
{
    auto* kq_impl = static_cast<kqueue_acceptor_impl*>(&impl);
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

    // Best-effort: failure only affects TIME_WAIT address reuse
    int reuse = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

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

    kq_impl->fd_ = fd;

    // Register fd with kqueue (edge-triggered via EV_CLEAR)
    kq_impl->desc_state_.fd = fd;
    {
        std::lock_guard lock(kq_impl->desc_state_.mutex);
        kq_impl->desc_state_.read_op = nullptr;
    }
    scheduler().register_descriptor(fd, &kq_impl->desc_state_);

    // Cache the local endpoint (queries OS for ephemeral port if port was 0)
    sockaddr_in local_addr{};
    socklen_t local_len = sizeof(local_addr);
    if (::getsockname(
            fd, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
        kq_impl->set_local_endpoint(detail::from_sockaddr_in(local_addr));

    return {};
}

void
kqueue_acceptor_service::post(kqueue_op* op)
{
    state_->sched_.post(op);
}

void
kqueue_acceptor_service::work_started() noexcept
{
    state_->sched_.work_started();
}

void
kqueue_acceptor_service::work_finished() noexcept
{
    state_->sched_.work_finished();
}

kqueue_socket_service*
kqueue_acceptor_service::socket_service() const noexcept
{
    auto* svc = ctx_.find_service<detail::socket_service>();
    return svc ? dynamic_cast<kqueue_socket_service*>(svc) : nullptr;
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE
