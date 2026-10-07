//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
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
#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/native/detail/reactor/reactor_io_core.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/endpoint_convert.hpp>

#include <cstring>
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
    , public reactor_io_core<Derived, Service, DescState>
    , public intrusive_list<Derived>::node
{
    friend Derived;

    using core_type = reactor_io_core<Derived, Service, DescState>;
    friend core_type;

protected:
    // NOLINTNEXTLINE(bugprone-crtp-constructor-accessibility)
    explicit reactor_acceptor(Service& svc) noexcept : core_type(svc) {}

protected:
    using core_type::svc_;
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

    using core_type::desc_state_;

    ~reactor_acceptor() override = default;

    /// Return the underlying file descriptor.
    native_handle_type native_handle() const noexcept override
    {
        return fd_;
    }

    corosio::family family() const noexcept override
    {
        return to_family(socket_family(fd_));
    }

    /// Release and return the native handle without closing it.
    native_handle_type release_socket() noexcept override
    {
        return do_release_socket();
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
        fd_            = fd;
        desc_state_.fd = fd;
        {
            std::lock_guard lock(desc_state_.mutex);
            desc_state_.read_op       = nullptr;
            desc_state_.wait_read_op  = nullptr;
            desc_state_.wait_write_op = nullptr;
            desc_state_.wait_error_op = nullptr;
        }
    }

    /** Assign the fd, initialize descriptor state, and register with
        the reactor.

        Adoption skips `do_listen`, so the registration it performs
        has to happen here instead.

        @param fd The already-listening descriptor to adopt.

        @return The error if the reactor rejects the descriptor, in
        which case the implementation is left closed and the caller
        retains ownership of @a fd; otherwise a default constructed
        error code.
    */
    std::error_code init_and_register(int fd) noexcept
    {
        fd_ = fd;
        if (auto ec = this->register_fd(fd))
        {
            fd_ = -1;
            return ec;
        }
        return {};
    }

    /// Return a reference to the owning service.
    Service& service() noexcept
    {
        return svc_;
    }

    void cancel() noexcept override
    {
        do_cancel();
    }

    /// Close the acceptor (non-virtual, called by the service).
    void close_socket() noexcept
    {
        do_close_socket();
    }

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

        For `wait_type::read`, completion signals that an incoming
        connection is pending and a subsequent accept succeeds
        without blocking; a connection already queued when the wait
        begins completes it immediately via an initiation probe.

        `wait_type::write` fails with `operation_not_supported` on
        every backend: writability carries no meaning for a
        listening socket.
    */
    std::coroutine_handle<> do_wait(
        std::coroutine_handle<>,
        capy::executor_ref,
        wait_type,
        std::stop_token const&,
        std::error_code*);

    /** Cancel the pending accept operation. */
    void do_cancel() noexcept
    {
        this->cancel_all();
    }

    /** Close the acceptor and cancel pending operations.

        Invoked by the derived class's close_socket(). The
        derived class may add backend-specific cleanup after
        calling this method.
    */
    void do_close_socket() noexcept
    {
        this->abandon_all();
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
        local_endpoint_ = Endpoint{};
    }

    /** Release the acceptor without closing the fd. */
    native_handle_type do_release_socket() noexcept
    {
        this->abandon_all();
        native_handle_type released = fd_;
        fd_                         = -1;
        local_endpoint_             = Endpoint{};
        return released;
    }

    /** Reset descriptor state for recycling.

        Actively re-initializes every field it owns rather than
        trusting `close_socket()`'s prior writes to still be there —
        see reactor_basic_socket::reuse() for the same rationale.
        `object_ref_` and `is_enqueued_` are asserted instead: `poison()`
        never touches them, so they must already hold their
        zero-action-complete state.

        @pre refs_ == 0, fd closed and deregistered, no op in flight.
    */
    void reuse() noexcept
    {
        fd_             = -1;
        local_endpoint_ = Endpoint{};
        this->reset_desc_state();
        BOOST_COROSIO_ASSERT(!acc_.stop_cb);
        BOOST_COROSIO_ASSERT(!wait_rd_.stop_cb);
        BOOST_COROSIO_ASSERT(!wait_wr_.stop_cb);
        BOOST_COROSIO_ASSERT(!wait_er_.stop_cb);
    }

#if !defined(NDEBUG)
    /** Poison the fields `reuse()` re-initializes, before the pool
        parks this impl on the free list. See
        reactor_basic_socket::poison() for the full rationale and the
        list of fields deliberately left untouched.

        @pre refs_ == 0, fd closed and deregistered, no op in flight.
    */
    void poison() noexcept
    {
        auto smash = [](auto& field) {
            std::memset(static_cast<void*>(&field), 0xDB, sizeof(field));
        };
        smash(fd_);
        smash(local_endpoint_);
        this->poison_desc_state();
    }
#endif

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
        @return The error code from listen() or from reactor
        registration, or success.
    */
    std::error_code do_listen(int backlog);

private:
    // CRTP callbacks for reactor_io_core cancel/close

    template<class AnyOp>
    reactor_op_base** op_to_desc_slot(AnyOp& op) noexcept
    {
        if (&op == static_cast<void*>(&acc_))
            return &desc_state_.read_op;
        if (&op == static_cast<void*>(&wait_rd_))
            return &desc_state_.wait_read_op;
        if (&op == static_cast<void*>(&wait_wr_))
            return &desc_state_.wait_write_op;
        if (&op == static_cast<void*>(&wait_er_))
            return &desc_state_.wait_error_op;
        return nullptr;
    }

    template<class Fn>
    void for_each_op(Fn fn) noexcept
    {
        fn(acc_);
        fn(wait_rd_);
        fn(wait_wr_);
        fn(wait_er_);
    }

    template<class Fn>
    void for_each_desc_entry(Fn fn) noexcept
    {
        fn(acc_, desc_state_.read_op);
        fn(wait_rd_, desc_state_.wait_read_op);
        fn(wait_wr_, desc_state_.wait_write_op);
        fn(wait_er_, desc_state_.wait_error_op);
    }
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
std::error_code
reactor_acceptor<
    Derived,
    Service,
    Op,
    AcceptOp,
    WaitOp,
    DescState,
    ImplBase,
    Endpoint>::do_bind(Endpoint const& ep)
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
reactor_acceptor<
    Derived,
    Service,
    Op,
    AcceptOp,
    WaitOp,
    DescState,
    ImplBase,
    Endpoint>::do_listen(int backlog)
{
    if (::listen(fd_, backlog) < 0)
        return make_err(errno);

    // A re-listen only changes the backlog; the descriptor is already
    // registered and re-adding it would fail on epoll.
    if (desc_state_.registered_events != 0)
        return {};

    return svc_.scheduler().register_descriptor(fd_, &desc_state_);
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
reactor_acceptor<
    Derived,
    Service,
    Op,
    AcceptOp,
    WaitOp,
    DescState,
    ImplBase,
    Endpoint>::
    do_wait(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        wait_type w,
        std::stop_token const& token,
        std::error_code* ec)
{
    // Writability carries no meaning for a listening socket; some
    // backends could only lie about it and others could never report
    // it, so the wait fails the same way everywhere instead.
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
        op.object_ref_ = detail::object_ref(this);
        op.complete(ENOTSUP, 0);
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

    auto& op = *op_ptr;
    op.reset();
    op.wait_event = event;
    op.h          = h;
    op.ex         = ex;
    op.ec_out     = ec;
    op.fd         = this->fd_;
    op.start(token, static_cast<Derived*>(this));
    op.object_ref_ = detail::object_ref(this);

    // A listener's readiness can predate the wait: an adopted or
    // shared descriptor has history the reactor never saw, and an
    // edge already dispatched will not be re-announced. Probe before
    // parking.
    int perr = 0;
    if (WaitOp::probe(this->fd_, event, perr))
    {
        op.complete(perr, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    svc_.work_started();

    // Posted after the descriptor lock is released: the scheduler
    // takes its own lock before descriptor locks.
    bool post_now = false;
    {
        std::lock_guard lock(desc_state_.mutex);
        if (op.cancelled.load(std::memory_order_acquire))
        {
            post_now = true;
        }
        else if (WaitOp::probe(this->fd_, event, perr))
        {
            // Close the probe-to-park window: an edge that landed after
            // the first probe was consumed, so re-check under the mutex
            // the dispatch path holds.
            op.complete(perr, 0);
            post_now = true;
        }
        else
        {
            *desc_slot_ptr = &op;
        }
    }
    if (post_now)
    {
        svc_.post(&op);
        svc_.work_finished();
    }
    else
    {
        // Select watches an fd only while an op is parked; see
        // register_op.
        if constexpr (Service::needs_park_notification)
            svc_.scheduler().notify_reactor();
    }
    return std::noop_coroutine();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_ACCEPTOR_HPP
