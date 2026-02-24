//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_ACCEPTOR_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_ACCEPTOR_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/detail/acceptor_service.hpp>

#include <boost/corosio/native/detail/epoll/epoll_acceptor.hpp>
#include <boost/corosio/native/detail/epoll/epoll_socket_service.hpp>
#include <boost/corosio/native/detail/epoll/epoll_scheduler.hpp>

#include <boost/corosio/detail/endpoint_convert.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/detail/make_err.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <errno.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

/** State for epoll acceptor service. */
class epoll_acceptor_state
{
public:
    explicit epoll_acceptor_state(epoll_scheduler& sched) noexcept
        : sched_(sched)
    {
    }

    epoll_scheduler& sched_;
    std::mutex mutex_;
    intrusive_list<epoll_acceptor> acceptor_list_;
    std::unordered_map<epoll_acceptor*, std::shared_ptr<epoll_acceptor>>
        acceptor_ptrs_;
};

/** epoll acceptor service implementation.

    Inherits from acceptor_service to enable runtime polymorphism.
    Uses key_type = acceptor_service for service lookup.
*/
class BOOST_COROSIO_DECL epoll_acceptor_service final : public acceptor_service
{
public:
    explicit epoll_acceptor_service(capy::execution_context& ctx);
    ~epoll_acceptor_service() override;

    epoll_acceptor_service(epoll_acceptor_service const&)            = delete;
    epoll_acceptor_service& operator=(epoll_acceptor_service const&) = delete;

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

    epoll_scheduler& scheduler() const noexcept
    {
        return state_->sched_;
    }
    void post(epoll_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

    /** Get the socket service for creating peer sockets during accept. */
    epoll_socket_service* socket_service() const noexcept;

private:
    capy::execution_context& ctx_;
    std::unique_ptr<epoll_acceptor_state> state_;
};

//--------------------------------------------------------------------------
//
// Implementation
//
//--------------------------------------------------------------------------

inline void
epoll_accept_op::cancel() noexcept
{
    if (acceptor_impl_)
        acceptor_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
epoll_accept_op::operator()()
{
    stop_cb.reset();

    static_cast<epoll_acceptor*>(acceptor_impl_)
        ->service()
        .scheduler()
        .reset_inline_budget();

    bool success = (errn == 0 && !cancelled.load(std::memory_order_acquire));

    if (cancelled.load(std::memory_order_acquire))
        *ec_out = capy::error::canceled;
    else if (errn != 0)
        *ec_out = make_err(errn);
    else
        *ec_out = {};

    // Set up the peer socket on success
    if (success && accepted_fd >= 0 && acceptor_impl_)
    {
        auto* socket_svc = static_cast<epoll_acceptor*>(acceptor_impl_)
                               ->service()
                               .socket_service();
        if (socket_svc)
        {
            auto& impl = static_cast<epoll_socket&>(*socket_svc->construct());
            impl.set_socket(accepted_fd);

            impl.desc_state_.fd = accepted_fd;
            {
                std::lock_guard lock(impl.desc_state_.mutex);
                impl.desc_state_.read_op    = nullptr;
                impl.desc_state_.write_op   = nullptr;
                impl.desc_state_.connect_op = nullptr;
            }
            socket_svc->scheduler().register_descriptor(
                accepted_fd, &impl.desc_state_);

            impl.set_endpoints(
                static_cast<epoll_acceptor*>(acceptor_impl_)->local_endpoint(),
                from_sockaddr(peer_storage));

            if (impl_out)
                *impl_out = &impl;
            accepted_fd = -1;
        }
        else
        {
            // No socket service — treat as error
            *ec_out = make_err(ENOENT);
            success = false;
        }
    }

    if (!success || !acceptor_impl_)
    {
        if (accepted_fd >= 0)
        {
            ::close(accepted_fd);
            accepted_fd = -1;
        }
        if (impl_out)
            *impl_out = nullptr;
    }

    // Move to stack before resuming. See epoll_op::operator()() for rationale.
    capy::executor_ref saved_ex(ex);
    std::coroutine_handle<> saved_h(h);
    auto prevent_premature_destruction = std::move(impl_ptr);
    dispatch_coro(saved_ex, saved_h).resume();
}

inline epoll_acceptor::epoll_acceptor(epoll_acceptor_service& svc) noexcept
    : svc_(svc)
{
}

inline std::coroutine_handle<>
epoll_acceptor::accept(
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
    int accepted;
    do
    {
        accepted = ::accept4(
            fd_, reinterpret_cast<sockaddr*>(&peer_storage), &addrlen,
            SOCK_NONBLOCK | SOCK_CLOEXEC);
    }
    while (accepted < 0 && errno == EINTR);

    if (accepted >= 0)
    {
        {
            std::lock_guard lock(desc_state_.mutex);
            desc_state_.read_ready = false;
        }

        if (svc_.scheduler().try_consume_inline_budget())
        {
            auto* socket_svc = svc_.socket_service();
            if (socket_svc)
            {
                auto& impl =
                    static_cast<epoll_socket&>(*socket_svc->construct());
                impl.set_socket(accepted);

                impl.desc_state_.fd = accepted;
                {
                    std::lock_guard lock(impl.desc_state_.mutex);
                    impl.desc_state_.read_op    = nullptr;
                    impl.desc_state_.write_op   = nullptr;
                    impl.desc_state_.connect_op = nullptr;
                }
                socket_svc->scheduler().register_descriptor(
                    accepted, &impl.desc_state_);

                impl.set_endpoints(
                    local_endpoint_, from_sockaddr(peer_storage));

                *ec = {};
                if (impl_out)
                    *impl_out = &impl;
            }
            else
            {
                ::close(accepted);
                *ec = make_err(ENOENT);
                if (impl_out)
                    *impl_out = nullptr;
            }
            return dispatch_coro(ex, h);
        }

        op.accepted_fd   = accepted;
        op.peer_storage  = peer_storage;
        op.complete(0, 0);
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return std::noop_coroutine();
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        op.impl_ptr = shared_from_this();
        svc_.work_started();

        std::lock_guard lock(desc_state_.mutex);
        bool io_done = false;
        if (desc_state_.read_ready)
        {
            desc_state_.read_ready = false;
            op.perform_io();
            io_done = (op.errn != EAGAIN && op.errn != EWOULDBLOCK);
            if (!io_done)
                op.errn = 0;
        }

        if (io_done || op.cancelled.load(std::memory_order_acquire))
        {
            svc_.post(&op);
            svc_.work_finished();
        }
        else
        {
            desc_state_.read_op = &op;
        }
        return std::noop_coroutine();
    }

    op.complete(errno, 0);
    op.impl_ptr = shared_from_this();
    svc_.post(&op);
    // completion is always posted to scheduler queue, never inline.
    return std::noop_coroutine();
}

inline void
epoll_acceptor::cancel() noexcept
{
    cancel_single_op(acc_);
}

inline void
epoll_acceptor::cancel_single_op(epoll_op& op) noexcept
{
    auto self = weak_from_this().lock();
    if (!self)
        return;

    op.request_cancel();

    epoll_op* claimed = nullptr;
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

inline void
epoll_acceptor::close_socket() noexcept
{
    auto self = weak_from_this().lock();
    if (self)
    {
        acc_.request_cancel();

        epoll_op* claimed = nullptr;
        {
            std::lock_guard lock(desc_state_.mutex);
            claimed = std::exchange(desc_state_.read_op, nullptr);
            desc_state_.read_ready  = false;
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

    desc_state_.fd                = -1;
    desc_state_.registered_events = 0;

    local_endpoint_ = endpoint{};
}

inline epoll_acceptor_service::epoll_acceptor_service(
    capy::execution_context& ctx)
    : ctx_(ctx)
    , state_(
          std::make_unique<epoll_acceptor_state>(
              ctx.use_service<epoll_scheduler>()))
{
}

inline epoll_acceptor_service::~epoll_acceptor_service() {}

inline void
epoll_acceptor_service::shutdown()
{
    std::lock_guard lock(state_->mutex_);

    while (auto* impl = state_->acceptor_list_.pop_front())
        impl->close_socket();

    // Don't clear acceptor_ptrs_ here — same rationale as
    // epoll_socket_service::shutdown(). Let ~state_ release ptrs
    // after scheduler shutdown has drained all queued ops.
}

inline io_object::implementation*
epoll_acceptor_service::construct()
{
    auto impl = std::make_shared<epoll_acceptor>(*this);
    auto* raw = impl.get();

    std::lock_guard lock(state_->mutex_);
    state_->acceptor_list_.push_back(raw);
    state_->acceptor_ptrs_.emplace(raw, std::move(impl));

    return raw;
}

inline void
epoll_acceptor_service::destroy(io_object::implementation* impl)
{
    auto* epoll_impl = static_cast<epoll_acceptor*>(impl);
    epoll_impl->close_socket();
    std::lock_guard lock(state_->mutex_);
    state_->acceptor_list_.remove(epoll_impl);
    state_->acceptor_ptrs_.erase(epoll_impl);
}

inline void
epoll_acceptor_service::close(io_object::handle& h)
{
    static_cast<epoll_acceptor*>(h.get())->close_socket();
}

inline std::error_code
epoll_acceptor::set_option(
    int level, int optname,
    void const* data, std::size_t size) noexcept
{
    if (::setsockopt(fd_, level, optname, data,
            static_cast<socklen_t>(size)) != 0)
        return make_err(errno);
    return {};
}

inline std::error_code
epoll_acceptor::get_option(
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
epoll_acceptor_service::open_acceptor_socket(
    tcp_acceptor::implementation& impl,
    int family, int type, int protocol)
{
    auto* epoll_impl = static_cast<epoll_acceptor*>(&impl);
    epoll_impl->close_socket();

    int fd = ::socket(family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
    if (fd < 0)
        return make_err(errno);

    if (family == AF_INET6)
    {
        int val = 0; // dual-stack default
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof(val));
    }

    epoll_impl->fd_ = fd;

    // Set up descriptor state but do NOT register with epoll yet
    epoll_impl->desc_state_.fd = fd;
    {
        std::lock_guard lock(epoll_impl->desc_state_.mutex);
        epoll_impl->desc_state_.read_op = nullptr;
    }

    return {};
}

inline std::error_code
epoll_acceptor_service::bind_acceptor(
    tcp_acceptor::implementation& impl, endpoint ep)
{
    auto* epoll_impl = static_cast<epoll_acceptor*>(&impl);
    int fd = epoll_impl->fd_;

    sockaddr_storage storage{};
    socklen_t addrlen = detail::to_sockaddr(ep, storage);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&storage), addrlen) < 0)
        return make_err(errno);

    // Cache local endpoint (resolves ephemeral port)
    sockaddr_storage local{};
    socklen_t local_len = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_len) == 0)
        epoll_impl->set_local_endpoint(detail::from_sockaddr(local));

    return {};
}

inline std::error_code
epoll_acceptor_service::listen_acceptor(
    tcp_acceptor::implementation& impl, int backlog)
{
    auto* epoll_impl = static_cast<epoll_acceptor*>(&impl);
    int fd = epoll_impl->fd_;

    if (::listen(fd, backlog) < 0)
        return make_err(errno);

    // Register fd with epoll (edge-triggered mode)
    scheduler().register_descriptor(fd, &epoll_impl->desc_state_);

    return {};
}

inline void
epoll_acceptor_service::post(epoll_op* op)
{
    state_->sched_.post(op);
}

inline void
epoll_acceptor_service::work_started() noexcept
{
    state_->sched_.work_started();
}

inline void
epoll_acceptor_service::work_finished() noexcept
{
    state_->sched_.work_finished();
}

inline epoll_socket_service*
epoll_acceptor_service::socket_service() const noexcept
{
    auto* svc = ctx_.find_service<detail::socket_service>();
    return svc ? dynamic_cast<epoll_socket_service*>(svc) : nullptr;
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_ACCEPTOR_SERVICE_HPP
