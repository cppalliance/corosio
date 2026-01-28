//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include "src/detail/epoll/acceptors.hpp"
#include "src/detail/epoll/sockets.hpp"
#include "src/detail/endpoint_convert.hpp"
#include "src/detail/make_err.hpp"

#include <errno.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

//------------------------------------------------------------------------------
// epoll_accept_op::cancel
//------------------------------------------------------------------------------

void
epoll_accept_op::
cancel() noexcept
{
    if (acceptor_impl_)
        acceptor_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

//------------------------------------------------------------------------------
// epoll_accept_op::operator() - creates peer socket and caches endpoints
//------------------------------------------------------------------------------

void
epoll_accept_op::
operator()()
{
    stop_cb.reset();

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
            auto* socket_svc = static_cast<epoll_acceptor_impl*>(acceptor_impl_)
                ->service().socket_service();
            if (socket_svc)
            {
                auto& impl = static_cast<epoll_socket_impl&>(socket_svc->create_impl());
                impl.set_socket(accepted_fd);

                sockaddr_in local_addr{};
                socklen_t local_len = sizeof(local_addr);
                sockaddr_in remote_addr{};
                socklen_t remote_len = sizeof(remote_addr);

                endpoint local_ep, remote_ep;
                if (::getsockname(accepted_fd, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
                    local_ep = from_sockaddr_in(local_addr);
                if (::getpeername(accepted_fd, reinterpret_cast<sockaddr*>(&remote_addr), &remote_len) == 0)
                    remote_ep = from_sockaddr_in(remote_addr);

                impl.set_endpoints(local_ep, remote_ep);

                if (impl_out)
                    *impl_out = &impl;

                accepted_fd = -1;
            }
            else
            {
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
            peer_impl->release();
            peer_impl = nullptr;
        }

        if (impl_out)
            *impl_out = nullptr;
    }

    // Move to stack before destroying the frame
    capy::executor_ref saved_ex( std::move( ex ) );
    capy::coro saved_h( std::move( h ) );
    impl_ptr.reset();
    saved_ex.dispatch( saved_h ).resume();
}

//------------------------------------------------------------------------------
// epoll_acceptor_impl
//------------------------------------------------------------------------------

epoll_acceptor_impl::
epoll_acceptor_impl(epoll_acceptor_service& svc) noexcept
    : svc_(svc)
{
}

void
epoll_acceptor_impl::
release()
{
    close_socket();
    svc_.destroy_acceptor_impl(*this);
}

void
epoll_acceptor_impl::
accept(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    std::stop_token token,
    system::error_code* ec,
    io_object::io_object_impl** impl_out)
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
    int accepted = ::accept4(fd_, reinterpret_cast<sockaddr*>(&addr),
                             &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (accepted >= 0)
    {
        op.accepted_fd = accepted;
        op.complete(0, 0);
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        svc_.work_started();
        // Set registering BEFORE register_fd to close the race window where
        // reactor sees an event before we set registered.
        op.registered.store(registration_state::registering, std::memory_order_release);
        svc_.scheduler().register_fd(fd_, &op, EPOLLIN | EPOLLET);

        // Transition to registered. If this fails, reactor or cancel already
        // claimed the op (state is now unregistered), so we're done. However,
        // we must still unregister the fd because cancel's unregister_fd may
        // have run before our register_fd, leaving the fd orphaned in epoll.
        auto expected = registration_state::registering;
        if (!op.registered.compare_exchange_strong(
                expected, registration_state::registered, std::memory_order_acq_rel))
        {
            svc_.scheduler().unregister_fd(fd_);
            return;
        }

        // If cancelled was set before we registered, handle it now.
        if (op.cancelled.load(std::memory_order_acquire))
        {
            auto prev = op.registered.exchange(
                registration_state::unregistered, std::memory_order_acq_rel);
            if (prev != registration_state::unregistered)
            {
                svc_.scheduler().unregister_fd(fd_);
                op.impl_ptr = shared_from_this();
                svc_.post(&op);
                svc_.work_finished();
            }
        }
        return;
    }

    op.complete(errno, 0);
    op.impl_ptr = shared_from_this();
    svc_.post(&op);
}

void
epoll_acceptor_impl::
cancel() noexcept
{
    std::shared_ptr<epoll_acceptor_impl> self;
    try {
        self = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return;
    }

    auto prev = acc_.registered.exchange(
        registration_state::unregistered, std::memory_order_acq_rel);
    acc_.request_cancel();

    if (prev != registration_state::unregistered)
    {
        svc_.scheduler().unregister_fd(fd_);
        acc_.impl_ptr = self;
        svc_.post(&acc_);
        svc_.work_finished();
    }
}

void
epoll_acceptor_impl::
cancel_single_op(epoll_op& op) noexcept
{
    // Called from stop_token callback to cancel a specific pending operation.
    auto prev = op.registered.exchange(
        registration_state::unregistered, std::memory_order_acq_rel);
    op.request_cancel();

    if (prev != registration_state::unregistered)
    {
        svc_.scheduler().unregister_fd(fd_);

        // Keep impl alive until op completes
        try {
            op.impl_ptr = shared_from_this();
        } catch (const std::bad_weak_ptr&) {
            // Impl is being destroyed, op will be orphaned but that's ok
        }

        svc_.post(&op);
        svc_.work_finished();
    }
}

void
epoll_acceptor_impl::
close_socket() noexcept
{
    cancel();

    if (fd_ >= 0)
    {
        // Unconditionally remove from epoll to handle edge cases
        svc_.scheduler().unregister_fd(fd_);
        ::close(fd_);
        fd_ = -1;
    }

    // Clear cached endpoint
    local_endpoint_ = endpoint{};
}

//------------------------------------------------------------------------------
// epoll_acceptor_service
//------------------------------------------------------------------------------

epoll_acceptor_service::
epoll_acceptor_service(capy::execution_context& ctx)
    : ctx_(ctx)
    , state_(std::make_unique<epoll_acceptor_state>(ctx.use_service<epoll_scheduler>()))
{
}

epoll_acceptor_service::
~epoll_acceptor_service()
{
}

void
epoll_acceptor_service::
shutdown()
{
    std::lock_guard lock(state_->mutex_);

    while (auto* impl = state_->acceptor_list_.pop_front())
        impl->close_socket();

    state_->acceptor_ptrs_.clear();
}

acceptor::acceptor_impl&
epoll_acceptor_service::
create_acceptor_impl()
{
    auto impl = std::make_shared<epoll_acceptor_impl>(*this);
    auto* raw = impl.get();

    std::lock_guard lock(state_->mutex_);
    state_->acceptor_list_.push_back(raw);
    state_->acceptor_ptrs_.emplace(raw, std::move(impl));

    return *raw;
}

void
epoll_acceptor_service::
destroy_acceptor_impl(acceptor::acceptor_impl& impl)
{
    auto* epoll_impl = static_cast<epoll_acceptor_impl*>(&impl);
    std::lock_guard lock(state_->mutex_);
    state_->acceptor_list_.remove(epoll_impl);
    state_->acceptor_ptrs_.erase(epoll_impl);
}

system::error_code
epoll_acceptor_service::
open_acceptor(
    acceptor::acceptor_impl& impl,
    endpoint ep,
    int backlog)
{
    auto* epoll_impl = static_cast<epoll_acceptor_impl*>(&impl);
    epoll_impl->close_socket();

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

    epoll_impl->fd_ = fd;

    // Cache the local endpoint (queries OS for ephemeral port if port was 0)
    sockaddr_in local_addr{};
    socklen_t local_len = sizeof(local_addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
        epoll_impl->set_local_endpoint(detail::from_sockaddr_in(local_addr));

    return {};
}

void
epoll_acceptor_service::
post(epoll_op* op)
{
    state_->sched_.post(op);
}

void
epoll_acceptor_service::
work_started() noexcept
{
    state_->sched_.work_started();
}

void
epoll_acceptor_service::
work_finished() noexcept
{
    state_->sched_.work_finished();
}

epoll_socket_service*
epoll_acceptor_service::
socket_service() const noexcept
{
    auto* svc = ctx_.find_service<detail::socket_service>();
    return svc ? dynamic_cast<epoll_socket_service*>(svc) : nullptr;
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL
