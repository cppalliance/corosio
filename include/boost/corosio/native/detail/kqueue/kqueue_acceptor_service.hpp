//
// Copyright (c) 2026 Michael Vandeberg
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_ACCEPTOR_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_ACCEPTOR_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/detail/acceptor_service.hpp>

#include <boost/corosio/native/detail/kqueue/kqueue_acceptor.hpp>
#include <boost/corosio/native/detail/kqueue/kqueue_socket_service.hpp>
#include <boost/corosio/native/detail/kqueue/kqueue_scheduler.hpp>

#include <boost/corosio/detail/endpoint_convert.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/detail/make_err.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

/** State for kqueue acceptor service. */
class kqueue_acceptor_state
{
    friend class kqueue_acceptor_service;

public:
    explicit kqueue_acceptor_state(kqueue_scheduler& sched) noexcept
        : sched_(sched)
    {
    }

private:
    kqueue_scheduler& sched_;
    std::mutex mutex_;
    intrusive_list<kqueue_acceptor> acceptor_list_;
    std::unordered_map<kqueue_acceptor*, std::shared_ptr<kqueue_acceptor>>
        acceptor_ptrs_;
};

/** kqueue acceptor service implementation.

    Inherits from acceptor_service to enable runtime polymorphism.
    Uses key_type = acceptor_service for service lookup.
*/
class BOOST_COROSIO_DECL kqueue_acceptor_service final : public acceptor_service
{
public:
    explicit kqueue_acceptor_service(capy::execution_context& ctx);
    ~kqueue_acceptor_service();

    kqueue_acceptor_service(kqueue_acceptor_service const&)            = delete;
    kqueue_acceptor_service& operator=(kqueue_acceptor_service const&) = delete;

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

    kqueue_scheduler& scheduler() const noexcept
    {
        return state_->sched_;
    }
    void post(kqueue_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

    /** Get the socket service for creating peer sockets during accept. */
    kqueue_socket_service* socket_service() const noexcept;

private:
    capy::execution_context& ctx_;
    std::unique_ptr<kqueue_acceptor_state> state_;
};

inline void
kqueue_accept_op::cancel() noexcept
{
    if (acceptor_impl_)
        acceptor_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
kqueue_accept_op::operator()()
{
    stop_cb.reset();

    static_cast<kqueue_acceptor*>(acceptor_impl_)
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
            auto* socket_svc = static_cast<kqueue_acceptor*>(acceptor_impl_)
                                   ->service()
                                   .socket_service();
            if (socket_svc)
            {
                auto& impl =
                    static_cast<kqueue_socket&>(*socket_svc->construct());
                impl.set_socket(accepted_fd);

                // Register accepted socket with kqueue (edge-triggered via EV_CLEAR)
                impl.desc_state_.fd = accepted_fd;
                {
                    std::lock_guard lock(impl.desc_state_.mutex);
                    impl.desc_state_.read_op    = nullptr;
                    impl.desc_state_.write_op   = nullptr;
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
                    socket_svc->destroy(&impl);
                    accepted_fd = -1;
                    if (impl_out)
                        *impl_out = nullptr;
                }
                else
                {
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
                static_cast<kqueue_acceptor*>(acceptor_impl_)
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

inline kqueue_acceptor::kqueue_acceptor(kqueue_acceptor_service& svc) noexcept
    : svc_(svc)
{
}

inline std::coroutine_handle<>
kqueue_acceptor::accept(
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

    // FreeBSD: Can use accept4(fd_, addr, addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC)
    int accepted =
        ::accept(fd_, reinterpret_cast<sockaddr*>(&peer_storage), &addrlen);

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

        if (svc_.scheduler().try_consume_inline_budget())
        {
            auto* socket_svc = svc_.socket_service();
            if (socket_svc)
            {
                auto& impl = static_cast<kqueue_socket&>(*socket_svc->construct());
                impl.set_socket(accepted);

                impl.desc_state_.fd = accepted;
                {
                    std::lock_guard lock(impl.desc_state_.mutex);
                    impl.desc_state_.read_op = nullptr;
                    impl.desc_state_.write_op = nullptr;
                    impl.desc_state_.connect_op = nullptr;
                }
                socket_svc->scheduler().register_descriptor(accepted, &impl.desc_state_);

                // Suppress SIGPIPE on the accepted socket; macOS lacks MSG_NOSIGNAL
                int one = 1;
                if (::setsockopt(accepted, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) == -1)
                {
                    int saved_errno = errno;
                    socket_svc->destroy(&impl);
                    if (ec)
                        *ec = make_err(saved_errno);
                    if (impl_out)
                        *impl_out = nullptr;
                }
                else
                {
                    sockaddr_storage local_storage{};
                    socklen_t local_len = sizeof(local_storage);
                    endpoint local_ep;
                    if (::getsockname(
                            accepted,
                            reinterpret_cast<sockaddr*>(&local_storage),
                            &local_len) == 0)
                        local_ep = from_sockaddr(local_storage);
                    impl.set_endpoints(
                        local_ep, from_sockaddr(peer_storage));
                    if (ec)
                        *ec = {};
                    if (impl_out)
                        *impl_out = &impl;
                }
                return dispatch_coro(ex, h);
            }
            else
            {
                ::close(accepted);
                if (ec)
                    *ec = make_err(ENOENT);
                if (impl_out)
                    *impl_out = nullptr;
                return dispatch_coro(ex, h);
            }
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
                perform_now            = true;
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

inline void
kqueue_acceptor::cancel() noexcept
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

inline void
kqueue_acceptor::cancel_single_op(kqueue_op& op) noexcept
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

inline void
kqueue_acceptor::close_socket() noexcept
{
    auto self = weak_from_this().lock();
    if (self)
    {
        acc_.request_cancel();

        kqueue_op* claimed = nullptr;
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
        ::close(fd_);
        fd_ = -1;
    }

    desc_state_.fd                = -1;
    desc_state_.registered_events = 0;

    local_endpoint_ = endpoint{};
}

inline kqueue_acceptor_service::kqueue_acceptor_service(
    capy::execution_context& ctx)
    : ctx_(ctx)
    , state_(
          std::make_unique<kqueue_acceptor_state>(
              ctx.use_service<kqueue_scheduler>()))
{
}

inline kqueue_acceptor_service::~kqueue_acceptor_service() = default;

inline void
kqueue_acceptor_service::shutdown()
{
    std::lock_guard lock(state_->mutex_);

    while (auto* impl = state_->acceptor_list_.pop_front())
        impl->close_socket();
}

inline io_object::implementation*
kqueue_acceptor_service::construct()
{
    auto impl = std::make_shared<kqueue_acceptor>(*this);
    auto* raw = impl.get();

    std::lock_guard lock(state_->mutex_);
    state_->acceptor_list_.push_back(raw);
    state_->acceptor_ptrs_.emplace(raw, std::move(impl));

    return raw;
}

inline void
kqueue_acceptor_service::destroy(io_object::implementation* impl)
{
    auto* kq_impl = static_cast<kqueue_acceptor*>(impl);
    kq_impl->close_socket();
    std::lock_guard lock(state_->mutex_);
    state_->acceptor_list_.remove(kq_impl);
    state_->acceptor_ptrs_.erase(kq_impl);
}

inline void
kqueue_acceptor_service::close(io_object::handle& h)
{
    static_cast<kqueue_acceptor*>(h.get())->close_socket();
}

inline std::error_code
kqueue_acceptor::set_option(
    int level, int optname,
    void const* data, std::size_t size) noexcept
{
    if (::setsockopt(fd_, level, optname, data,
            static_cast<socklen_t>(size)) != 0)
        return make_err(errno);
    return {};
}

inline std::error_code
kqueue_acceptor::get_option(
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
kqueue_acceptor_service::open_acceptor_socket(
    tcp_acceptor::implementation& impl,
    int family, int type, int protocol)
{
    auto* kq_impl = static_cast<kqueue_acceptor*>(&impl);
    kq_impl->close_socket();

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

    if (family == AF_INET6)
    {
        int val = 0; // dual-stack default
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof(val));
    }

    // SO_NOSIGPIPE on macOS (where MSG_NOSIGNAL doesn't exist)
#ifdef SO_NOSIGPIPE
    int nosig = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif

    kq_impl->fd_ = fd;

    // Set up descriptor state but do NOT register with kqueue yet
    kq_impl->desc_state_.fd = fd;
    {
        std::lock_guard lock(kq_impl->desc_state_.mutex);
        kq_impl->desc_state_.read_op = nullptr;
    }

    return {};
}

inline std::error_code
kqueue_acceptor_service::bind_acceptor(
    tcp_acceptor::implementation& impl, endpoint ep)
{
    auto* kq_impl = static_cast<kqueue_acceptor*>(&impl);
    int fd = kq_impl->fd_;

    sockaddr_storage storage{};
    socklen_t addrlen = detail::to_sockaddr(ep, storage);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&storage), addrlen) < 0)
        return make_err(errno);

    // Cache local endpoint (resolves ephemeral port)
    sockaddr_storage local{};
    socklen_t local_len = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_len) == 0)
        kq_impl->set_local_endpoint(detail::from_sockaddr(local));

    return {};
}

inline std::error_code
kqueue_acceptor_service::listen_acceptor(
    tcp_acceptor::implementation& impl, int backlog)
{
    auto* kq_impl = static_cast<kqueue_acceptor*>(&impl);
    int fd = kq_impl->fd_;

    if (::listen(fd, backlog) < 0)
        return make_err(errno);

    // Register fd with kqueue
    scheduler().register_descriptor(fd, &kq_impl->desc_state_);

    return {};
}

inline void
kqueue_acceptor_service::post(kqueue_op* op)
{
    state_->sched_.post(op);
}

inline void
kqueue_acceptor_service::work_started() noexcept
{
    state_->sched_.work_started();
}

inline void
kqueue_acceptor_service::work_finished() noexcept
{
    state_->sched_.work_finished();
}

inline kqueue_socket_service*
kqueue_acceptor_service::socket_service() const noexcept
{
    auto* svc = ctx_.find_service<detail::socket_service>();
    return svc ? dynamic_cast<kqueue_socket_service*>(svc) : nullptr;
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_ACCEPTOR_SERVICE_HPP
