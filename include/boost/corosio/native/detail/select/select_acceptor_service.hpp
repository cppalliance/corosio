//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_ACCEPTOR_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_ACCEPTOR_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/detail/acceptor_service.hpp>

#include <boost/corosio/native/detail/select/select_acceptor.hpp>
#include <boost/corosio/native/detail/select/select_socket_service.hpp>
#include <boost/corosio/native/detail/select/select_scheduler.hpp>

#include <boost/corosio/detail/endpoint_convert.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/detail/make_err.hpp>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace boost::corosio::detail {

/** State for select acceptor service. */
class select_acceptor_state
{
public:
    explicit select_acceptor_state(select_scheduler& sched) noexcept
        : sched_(sched)
    {
    }

    select_scheduler& sched_;
    std::mutex mutex_;
    intrusive_list<select_acceptor> acceptor_list_;
    std::unordered_map<select_acceptor*, std::shared_ptr<select_acceptor>>
        acceptor_ptrs_;
};

/** select acceptor service implementation.

    Inherits from acceptor_service to enable runtime polymorphism.
    Uses key_type = acceptor_service for service lookup.
*/
class BOOST_COROSIO_DECL select_acceptor_service final : public acceptor_service
{
public:
    explicit select_acceptor_service(capy::execution_context& ctx);
    ~select_acceptor_service() override;

    select_acceptor_service(select_acceptor_service const&)            = delete;
    select_acceptor_service& operator=(select_acceptor_service const&) = delete;

    void shutdown() override;

    io_object::implementation* construct() override;
    void destroy(io_object::implementation*) override;
    void close(io_object::handle&) override;
    std::error_code open_acceptor_socket(
        tcp_acceptor::implementation& impl,
        int family, int type, int protocol) override;
    std::error_code bind_acceptor(
        tcp_acceptor::implementation& impl, endpoint ep) override;
    std::error_code listen_acceptor(
        tcp_acceptor::implementation& impl, int backlog) override;

    select_scheduler& scheduler() const noexcept
    {
        return state_->sched_;
    }
    void post(select_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

    /** Get the socket service for creating peer sockets during accept. */
    select_socket_service* socket_service() const noexcept;

private:
    capy::execution_context& ctx_;
    std::unique_ptr<select_acceptor_state> state_;
};

inline void
select_accept_op::cancel() noexcept
{
    if (acceptor_impl_)
        acceptor_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
select_accept_op::operator()()
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
            auto* socket_svc = static_cast<select_acceptor*>(acceptor_impl_)
                                   ->service()
                                   .socket_service();
            if (socket_svc)
            {
                auto& impl =
                    static_cast<select_socket&>(*socket_svc->construct());
                impl.set_socket(accepted_fd);

                sockaddr_storage local_storage{};
                socklen_t local_len = sizeof(local_storage);
                sockaddr_storage remote_storage{};
                socklen_t remote_len = sizeof(remote_storage);

                endpoint local_ep, remote_ep;
                if (::getsockname(
                        accepted_fd,
                        reinterpret_cast<sockaddr*>(&local_storage),
                        &local_len) == 0)
                    local_ep = from_sockaddr(local_storage);
                if (::getpeername(
                        accepted_fd,
                        reinterpret_cast<sockaddr*>(&remote_storage),
                        &remote_len) == 0)
                    remote_ep = from_sockaddr(remote_storage);

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
            auto* socket_svc_cleanup =
                static_cast<select_acceptor*>(acceptor_impl_)
                    ->service()
                    .socket_service();
            if (socket_svc_cleanup)
                socket_svc_cleanup->destroy(peer_impl);
            peer_impl = nullptr;
        }

        if (impl_out)
            *impl_out = nullptr;
    }

    // Move to stack before destroying the frame
    capy::executor_ref saved_ex(ex);
    std::coroutine_handle<> saved_h(h);
    impl_ptr.reset();
    dispatch_coro(saved_ex, saved_h).resume();
}

inline select_acceptor::select_acceptor(select_acceptor_service& svc) noexcept
    : svc_(svc)
{
}

inline std::coroutine_handle<>
select_acceptor::accept(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    std::stop_token token,
    std::error_code* ec,
    io_object::implementation** impl_out)
{
    auto& op = acc_;
    op.reset();
    op.h        = h;
    op.ex       = ex;
    op.ec_out   = ec;
    op.impl_out = impl_out;
    op.fd       = fd_;
    op.start(token, this);

    sockaddr_storage peer_storage{};
    socklen_t addrlen = sizeof(peer_storage);
    int accepted =
        ::accept(fd_, reinterpret_cast<sockaddr*>(&peer_storage), &addrlen);

    if (accepted >= 0)
    {
        // Reject fds that exceed select()'s FD_SETSIZE limit.
        if (accepted >= FD_SETSIZE)
        {
            ::close(accepted);
            op.accepted_fd = -1;
            op.complete(EINVAL, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return std::noop_coroutine();
        }

        // Set non-blocking and close-on-exec flags.
        int flags = ::fcntl(accepted, F_GETFL, 0);
        if (flags == -1)
        {
            int err = errno;
            ::close(accepted);
            op.accepted_fd = -1;
            op.complete(err, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return std::noop_coroutine();
        }

        if (::fcntl(accepted, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            int err = errno;
            ::close(accepted);
            op.accepted_fd = -1;
            op.complete(err, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return std::noop_coroutine();
        }

        if (::fcntl(accepted, F_SETFD, FD_CLOEXEC) == -1)
        {
            int err = errno;
            ::close(accepted);
            op.accepted_fd = -1;
            op.complete(err, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return std::noop_coroutine();
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

inline void
select_acceptor::cancel() noexcept
{
    auto self = weak_from_this().lock();
    if (!self)
        return;

    auto prev = acc_.registered.exchange(
        select_registration_state::unregistered, std::memory_order_acq_rel);
    acc_.request_cancel();

    if (prev != select_registration_state::unregistered)
    {
        svc_.scheduler().deregister_fd(fd_, select_scheduler::event_read);
        acc_.impl_ptr = self;
        svc_.post(&acc_);
        svc_.work_finished();
    }
}

inline void
select_acceptor::cancel_single_op(select_op& op) noexcept
{
    auto self = weak_from_this().lock();
    if (!self)
        return;

    auto prev = op.registered.exchange(
        select_registration_state::unregistered, std::memory_order_acq_rel);
    op.request_cancel();

    if (prev != select_registration_state::unregistered)
    {
        svc_.scheduler().deregister_fd(fd_, select_scheduler::event_read);

        op.impl_ptr = self;
        svc_.post(&op);
        svc_.work_finished();
    }
}

inline void
select_acceptor::close_socket() noexcept
{
    auto self = weak_from_this().lock();
    if (self)
    {
        auto prev = acc_.registered.exchange(
            select_registration_state::unregistered, std::memory_order_acq_rel);
        acc_.request_cancel();

        if (prev != select_registration_state::unregistered)
        {
            svc_.scheduler().deregister_fd(fd_, select_scheduler::event_read);
            acc_.impl_ptr = self;
            svc_.post(&acc_);
            svc_.work_finished();
        }
    }

    if (fd_ >= 0)
    {
        svc_.scheduler().deregister_fd(fd_, select_scheduler::event_read);
        ::close(fd_);
        fd_ = -1;
    }

    local_endpoint_ = endpoint{};
}

inline select_acceptor_service::select_acceptor_service(
    capy::execution_context& ctx)
    : ctx_(ctx)
    , state_(
          std::make_unique<select_acceptor_state>(
              ctx.use_service<select_scheduler>()))
{
}

inline select_acceptor_service::~select_acceptor_service() {}

inline void
select_acceptor_service::shutdown()
{
    std::lock_guard lock(state_->mutex_);

    while (auto* impl = state_->acceptor_list_.pop_front())
        impl->close_socket();

    // Don't clear acceptor_ptrs_ here — same rationale as
    // select_socket_service::shutdown(). Let ~state_ release ptrs
    // after scheduler shutdown has drained all queued ops.
}

inline io_object::implementation*
select_acceptor_service::construct()
{
    auto impl = std::make_shared<select_acceptor>(*this);
    auto* raw = impl.get();

    std::lock_guard lock(state_->mutex_);
    state_->acceptor_list_.push_back(raw);
    state_->acceptor_ptrs_.emplace(raw, std::move(impl));

    return raw;
}

inline void
select_acceptor_service::destroy(io_object::implementation* impl)
{
    auto* select_impl = static_cast<select_acceptor*>(impl);
    select_impl->close_socket();
    std::lock_guard lock(state_->mutex_);
    state_->acceptor_list_.remove(select_impl);
    state_->acceptor_ptrs_.erase(select_impl);
}

inline void
select_acceptor_service::close(io_object::handle& h)
{
    static_cast<select_acceptor*>(h.get())->close_socket();
}

inline std::error_code
select_acceptor::set_option(
    int level, int optname,
    void const* data, std::size_t size) noexcept
{
    if (::setsockopt(fd_, level, optname, data,
            static_cast<socklen_t>(size)) != 0)
        return make_err(errno);
    return {};
}

inline std::error_code
select_acceptor::get_option(
    int level, int optname,
    void* data, std::size_t* size) const noexcept
{
    socklen_t len = static_cast<socklen_t>(*size);
    if (::getsockopt(fd_, level, optname, data, &len) != 0)
        return make_err(errno);
    *size = static_cast<std::size_t>(len);
    return {};
}

inline std::error_code
select_acceptor_service::open_acceptor_socket(
    tcp_acceptor::implementation& impl,
    int family, int type, int protocol)
{
    auto* select_impl = static_cast<select_acceptor*>(&impl);
    select_impl->close_socket();

    int fd = ::socket(family, type, protocol);
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

    if (fd >= FD_SETSIZE)
    {
        ::close(fd);
        return make_err(EMFILE);
    }

    if (family == AF_INET6)
    {
        int val = 0; // dual-stack default
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof(val));
    }

    select_impl->fd_ = fd;
    return {};
}

inline std::error_code
select_acceptor_service::bind_acceptor(
    tcp_acceptor::implementation& impl, endpoint ep)
{
    auto* select_impl = static_cast<select_acceptor*>(&impl);
    int fd = select_impl->fd_;

    sockaddr_storage storage{};
    socklen_t addrlen = detail::to_sockaddr(ep, storage);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&storage), addrlen) < 0)
        return make_err(errno);

    // Cache local endpoint (resolves ephemeral port)
    sockaddr_storage local{};
    socklen_t local_len = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_len) == 0)
        select_impl->set_local_endpoint(detail::from_sockaddr(local));

    return {};
}

inline std::error_code
select_acceptor_service::listen_acceptor(
    tcp_acceptor::implementation& impl, int backlog)
{
    auto* select_impl = static_cast<select_acceptor*>(&impl);
    int fd = select_impl->fd_;

    if (::listen(fd, backlog) < 0)
        return make_err(errno);

    return {};
}

inline void
select_acceptor_service::post(select_op* op)
{
    state_->sched_.post(op);
}

inline void
select_acceptor_service::work_started() noexcept
{
    state_->sched_.work_started();
}

inline void
select_acceptor_service::work_finished() noexcept
{
    state_->sched_.work_finished();
}

inline select_socket_service*
select_acceptor_service::socket_service() const noexcept
{
    auto* svc = ctx_.find_service<detail::socket_service>();
    return svc ? dynamic_cast<select_socket_service*>(svc) : nullptr;
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_ACCEPTOR_SERVICE_HPP
