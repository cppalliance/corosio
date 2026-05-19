//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_ACCEPTOR_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_ACCEPTOR_HPP

#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op_base.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/endpoint_convert.hpp>

#include <memory>
#include <mutex>
#include <utility>

#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

/** CRTP base for reactor-backed acceptor implementations.

    Provides shared data members, trivial virtual overrides, and
    non-virtual helper methods for cancellation and close. Concrete
    backends inherit and add `cancel()`, `close_socket()`, and
    `accept()` overrides that delegate to the `do_*` helpers.

    @tparam Derived   The concrete acceptor type (CRTP).
    @tparam Service   The backend's acceptor service type.
    @tparam Op        The backend's base op type.
    @tparam AcceptOp  The backend's accept op type.
    @tparam WaitOp    The backend's wait op type.
    @tparam DescState The backend's descriptor_state type.
    @tparam ImplBase  The public vtable base
                      (tcp_acceptor::implementation or
                       local_stream_acceptor::implementation).
    @tparam Endpoint  The endpoint type (endpoint or local_endpoint).
*/
template<
    class Derived,
    class Service,
    class Op,
    class AcceptOp,
    class WaitOp,
    class DescState,
    class ImplBase = tcp_acceptor::implementation,
    class Endpoint = endpoint>
class reactor_acceptor
    : public ImplBase
    , public std::enable_shared_from_this<Derived>
    , public intrusive_list<Derived>::node
{
    friend Derived;

protected:
    // NOLINTNEXTLINE(bugprone-crtp-constructor-accessibility)
    explicit reactor_acceptor(Service& svc) noexcept : svc_(svc) {}

protected:
    Service& svc_;
    int fd_ = -1;
    Endpoint local_endpoint_;

public:
    /// Pending accept operation slot.
    AcceptOp acc_;

    /// Pending wait-for-read operation slot.
    WaitOp wait_rd_;

    /// Pending wait-for-write operation slot.
    WaitOp wait_wr_;

    /// Pending wait-for-error operation slot.
    WaitOp wait_er_;

    /// Per-descriptor state for persistent reactor registration.
    DescState desc_state_;

    ~reactor_acceptor() override = default;

    /// Return the underlying file descriptor.
    int native_handle() const noexcept
    {
        return fd_;
    }

    /// Return the cached local endpoint.
    Endpoint local_endpoint() const noexcept override
    {
        return local_endpoint_;
    }

    /// Return true if the acceptor has an open file descriptor.
    bool is_open() const noexcept override
    {
        return fd_ >= 0;
    }

    /// Set a socket option.
    std::error_code set_option(
        int level,
        int optname,
        void const* data,
        std::size_t size) noexcept override
    {
        if (::setsockopt(
                fd_, level, optname, data, static_cast<socklen_t>(size)) != 0)
            return make_err(errno);
        return {};
    }

    /// Get a socket option.
    std::error_code
    get_option(int level, int optname, void* data, std::size_t* size)
        const noexcept override
    {
        socklen_t len = static_cast<socklen_t>(*size);
        if (::getsockopt(fd_, level, optname, data, &len) != 0)
            return make_err(errno);
        *size = static_cast<std::size_t>(len);
        return {};
    }

    /// Cache the local endpoint.
    void set_local_endpoint(Endpoint ep) noexcept
    {
        local_endpoint_ = std::move(ep);
    }

    /// Assign the fd and initialize descriptor state for the acceptor.
    void init_acceptor_fd(int fd) noexcept
    {
        fd_ = fd;
        desc_state_.fd = fd;
        {
            std::lock_guard lock(desc_state_.mutex);
            desc_state_.read_op       = nullptr;
            desc_state_.wait_read_op  = nullptr;
            desc_state_.wait_write_op = nullptr;
            desc_state_.wait_error_op = nullptr;
        }
    }

    /// Return a reference to the owning service.
    Service& service() noexcept
    {
        return svc_;
    }

    void cancel() noexcept override { do_cancel(); }

    /// Close the acceptor (non-virtual, called by the service).
    void close_socket() noexcept { do_close_socket(); }

    std::coroutine_handle<> wait(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        wait_type w,
        std::stop_token token,
        std::error_code* ec) override
    {
        return do_wait(h, ex, w, token, ec);
    }

    /** Wait for readiness on the listen socket.

        Registers a wait op on the matching event slot. For
        `wait_type::read`, completion signals that an incoming
        connection is pending and a subsequent accept will
        succeed without blocking.
    */
    std::coroutine_handle<> do_wait(
        std::coroutine_handle<>,
        capy::executor_ref,
        wait_type,
        std::stop_token const&,
        std::error_code*);

    /** Cancel a single pending operation.

        Claims the operation from the read_op descriptor slot
        under the mutex and posts it to the scheduler as cancelled.

        @param op The operation to cancel.
    */
    void cancel_single_op(Op& op) noexcept;

    /** Cancel the pending accept operation. */
    void do_cancel() noexcept;

    /** Close the acceptor and cancel pending operations.

        Invoked by the derived class's close_socket(). The
        derived class may add backend-specific cleanup after
        calling this method.
    */
    void do_close_socket() noexcept;

    /** Release the acceptor without closing the fd. */
    native_handle_type do_release_socket() noexcept;

    /** Bind the acceptor socket to an endpoint.

        Caches the resolved local endpoint (including ephemeral
        port) after a successful bind.

        @param ep The endpoint to bind to.
        @return The error code from bind(), or success.
    */
    std::error_code do_bind(Endpoint const& ep);

    /** Start listening on the acceptor socket.

        Registers the file descriptor with the reactor after
        a successful listen() call.

        @param backlog The listen backlog.
        @return The error code from listen(), or success.
    */
    std::error_code do_listen(int backlog);
};

template<
    class Derived,
    class Service,
    class Op,
    class AcceptOp,
    class WaitOp,
    class DescState,
    class ImplBase,
    class Endpoint>
void
reactor_acceptor<Derived, Service, Op, AcceptOp, WaitOp, DescState, ImplBase, Endpoint>::
    cancel_single_op(Op& op) noexcept
{
    auto self = this->weak_from_this().lock();
    if (!self)
        return;

    op.request_cancel();

    reactor_op_base* claimed = nullptr;
    {
        std::lock_guard lock(desc_state_.mutex);
        auto try_claim = [&](reactor_op_base*& slot) {
            if (!claimed && slot == &op)
                claimed = std::exchange(slot, nullptr);
        };
        try_claim(desc_state_.read_op);
        try_claim(desc_state_.wait_read_op);
        try_claim(desc_state_.wait_write_op);
        try_claim(desc_state_.wait_error_op);
    }
    if (claimed)
    {
        op.impl_ptr = self;
        svc_.post(&op);
        svc_.work_finished();
    }
}

template<
    class Derived,
    class Service,
    class Op,
    class AcceptOp,
    class WaitOp,
    class DescState,
    class ImplBase,
    class Endpoint>
void
reactor_acceptor<Derived, Service, Op, AcceptOp, WaitOp, DescState, ImplBase, Endpoint>::
    do_cancel() noexcept
{
    cancel_single_op(acc_);
    cancel_single_op(wait_rd_);
    cancel_single_op(wait_wr_);
    cancel_single_op(wait_er_);
}

template<
    class Derived,
    class Service,
    class Op,
    class AcceptOp,
    class WaitOp,
    class DescState,
    class ImplBase,
    class Endpoint>
void
reactor_acceptor<Derived, Service, Op, AcceptOp, WaitOp, DescState, ImplBase, Endpoint>::
    do_close_socket() noexcept
{
    auto self = this->weak_from_this().lock();
    if (self)
    {
        acc_.request_cancel();
        wait_rd_.request_cancel();
        wait_wr_.request_cancel();
        wait_er_.request_cancel();

        reactor_op_base* claimed_acc = nullptr;
        reactor_op_base* claimed_wr  = nullptr;
        reactor_op_base* claimed_ww  = nullptr;
        reactor_op_base* claimed_we  = nullptr;
        {
            std::lock_guard lock(desc_state_.mutex);
            claimed_acc = std::exchange(desc_state_.read_op, nullptr);
            claimed_wr  = std::exchange(desc_state_.wait_read_op, nullptr);
            claimed_ww  = std::exchange(desc_state_.wait_write_op, nullptr);
            claimed_we  = std::exchange(desc_state_.wait_error_op, nullptr);
            desc_state_.read_ready  = false;
            desc_state_.write_ready = false;

            if (desc_state_.is_enqueued_.load(std::memory_order_acquire))
                desc_state_.impl_ref_ = self;
        }

        auto repost = [&](reactor_op_base* claimed, reactor_op_base& op) {
            if (claimed)
            {
                op.impl_ptr = self;
                svc_.post(&op);
                svc_.work_finished();
            }
        };
        repost(claimed_acc, acc_);
        repost(claimed_wr, wait_rd_);
        repost(claimed_ww, wait_wr_);
        repost(claimed_we, wait_er_);
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

    local_endpoint_ = Endpoint{};
}

template<
    class Derived,
    class Service,
    class Op,
    class AcceptOp,
    class WaitOp,
    class DescState,
    class ImplBase,
    class Endpoint>
native_handle_type
reactor_acceptor<Derived, Service, Op, AcceptOp, WaitOp, DescState, ImplBase, Endpoint>::
    do_release_socket() noexcept
{
    auto self = this->weak_from_this().lock();
    if (self)
    {
        acc_.request_cancel();
        wait_rd_.request_cancel();
        wait_wr_.request_cancel();
        wait_er_.request_cancel();

        reactor_op_base* claimed_acc = nullptr;
        reactor_op_base* claimed_wr  = nullptr;
        reactor_op_base* claimed_ww  = nullptr;
        reactor_op_base* claimed_we  = nullptr;
        {
            std::lock_guard lock(desc_state_.mutex);
            claimed_acc = std::exchange(desc_state_.read_op, nullptr);
            claimed_wr  = std::exchange(desc_state_.wait_read_op, nullptr);
            claimed_ww  = std::exchange(desc_state_.wait_write_op, nullptr);
            claimed_we  = std::exchange(desc_state_.wait_error_op, nullptr);
            desc_state_.read_ready  = false;
            desc_state_.write_ready = false;

            if (desc_state_.is_enqueued_.load(std::memory_order_acquire))
                desc_state_.impl_ref_ = self;
        }

        auto repost = [&](reactor_op_base* claimed, reactor_op_base& op) {
            if (claimed)
            {
                op.impl_ptr = self;
                svc_.post(&op);
                svc_.work_finished();
            }
        };
        repost(claimed_acc, acc_);
        repost(claimed_wr, wait_rd_);
        repost(claimed_ww, wait_wr_);
        repost(claimed_we, wait_er_);
    }

    native_handle_type released = fd_;

    if (fd_ >= 0)
    {
        if (desc_state_.registered_events != 0)
            svc_.scheduler().deregister_descriptor(fd_);
        fd_ = -1;
    }

    desc_state_.fd                = -1;
    desc_state_.registered_events = 0;

    local_endpoint_ = Endpoint{};

    return released;
}

template<
    class Derived,
    class Service,
    class Op,
    class AcceptOp,
    class WaitOp,
    class DescState,
    class ImplBase,
    class Endpoint>
std::error_code
reactor_acceptor<Derived, Service, Op, AcceptOp, WaitOp, DescState, ImplBase, Endpoint>::
    do_bind(Endpoint const& ep)
{
    sockaddr_storage storage{};
    socklen_t addrlen = to_sockaddr(ep, storage);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&storage), addrlen) < 0)
        return make_err(errno);

    // Cache local endpoint (resolves ephemeral port / path)
    sockaddr_storage local{};
    socklen_t local_len = sizeof(local);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&local), &local_len) ==
        0)
        set_local_endpoint(from_sockaddr_as(local, local_len, Endpoint{}));

    return {};
}

template<
    class Derived,
    class Service,
    class Op,
    class AcceptOp,
    class WaitOp,
    class DescState,
    class ImplBase,
    class Endpoint>
std::error_code
reactor_acceptor<Derived, Service, Op, AcceptOp, WaitOp, DescState, ImplBase, Endpoint>::
    do_listen(int backlog)
{
    if (::listen(fd_, backlog) < 0)
        return make_err(errno);

    svc_.scheduler().register_descriptor(fd_, &desc_state_);
    return {};
}

template<
    class Derived,
    class Service,
    class Op,
    class AcceptOp,
    class WaitOp,
    class DescState,
    class ImplBase,
    class Endpoint>
std::coroutine_handle<>
reactor_acceptor<Derived, Service, Op, AcceptOp, WaitOp, DescState, ImplBase, Endpoint>::
    do_wait(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        wait_type w,
        std::stop_token const& token,
        std::error_code* ec)
{
    // wait_type::write completes immediately (see reactor_stream_socket::do_wait).
    if (w == wait_type::write)
    {
        auto& op = wait_wr_;
        op.reset();
        op.wait_event = reactor_event_write;
        op.h          = h;
        op.ex         = ex;
        op.ec_out     = ec;
        op.fd         = this->fd_;
        op.start(token, static_cast<Derived*>(this));
        op.impl_ptr   = this->shared_from_this();
        op.complete(0, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    WaitOp* op_ptr;
    reactor_op_base** desc_slot_ptr;
    std::uint32_t event;

    if (w == wait_type::read)
    {
        op_ptr        = &wait_rd_;
        desc_slot_ptr = &desc_state_.wait_read_op;
        event         = reactor_event_read;
    }
    else // wait_type::error
    {
        op_ptr        = &wait_er_;
        desc_slot_ptr = &desc_state_.wait_error_op;
        event         = reactor_event_error;
    }

    auto& op      = *op_ptr;
    op.reset();
    op.wait_event = event;
    op.h          = h;
    op.ex         = ex;
    op.ec_out     = ec;
    op.fd         = this->fd_;
    op.start(token, static_cast<Derived*>(this));
    op.impl_ptr   = this->shared_from_this();

    svc_.work_started();

    std::lock_guard lock(desc_state_.mutex);
    if (op.cancelled.load(std::memory_order_acquire))
    {
        svc_.post(&op);
        svc_.work_finished();
    }
    else
    {
        *desc_slot_ptr = &op;
    }
    return std::noop_coroutine();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_ACCEPTOR_HPP
