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
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op_base.hpp>
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
    class DescState,
    class ImplBase = tcp_acceptor::implementation,
    class Endpoint = endpoint>
class reactor_acceptor
    : public ImplBase
    , public std::enable_shared_from_this<Derived>
    , public intrusive_list<Derived>::node
{
    friend Derived;

    explicit reactor_acceptor(Service& svc) noexcept : svc_(svc) {}

protected:
    Service& svc_;
    int fd_ = -1;
    Endpoint local_endpoint_;

public:
    /// Pending accept operation slot.
    AcceptOp acc_;

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

    /// Return a reference to the owning service.
    Service& service() noexcept
    {
        return svc_;
    }

    /** Cancel a single pending operation.

        Claims the operation from the read_op descriptor slot
        under the mutex and posts it to the scheduler as cancelled.

        @param op The operation to cancel.
    */
    void cancel_single_op(Op& op) noexcept;

    /** Cancel the pending accept operation.

        Invoked by the derived class's cancel() override.
    */
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
    class DescState,
    class ImplBase,
    class Endpoint>
void
reactor_acceptor<Derived, Service, Op, AcceptOp, DescState, ImplBase, Endpoint>::
    cancel_single_op(Op& op) noexcept
{
    auto self = this->weak_from_this().lock();
    if (!self)
        return;

    op.request_cancel();

    reactor_op_base* claimed = nullptr;
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

template<
    class Derived,
    class Service,
    class Op,
    class AcceptOp,
    class DescState,
    class ImplBase,
    class Endpoint>
void
reactor_acceptor<Derived, Service, Op, AcceptOp, DescState, ImplBase, Endpoint>::
    do_cancel() noexcept
{
    cancel_single_op(acc_);
}

template<
    class Derived,
    class Service,
    class Op,
    class AcceptOp,
    class DescState,
    class ImplBase,
    class Endpoint>
void
reactor_acceptor<Derived, Service, Op, AcceptOp, DescState, ImplBase, Endpoint>::
    do_close_socket() noexcept
{
    auto self = this->weak_from_this().lock();
    if (self)
    {
        acc_.request_cancel();

        reactor_op_base* claimed = nullptr;
        {
            std::lock_guard lock(desc_state_.mutex);
            claimed = std::exchange(desc_state_.read_op, nullptr);
            desc_state_.read_ready  = false;
            desc_state_.write_ready = false;

            if (desc_state_.is_enqueued_.load(std::memory_order_acquire))
                desc_state_.impl_ref_ = self;
        }

        if (claimed)
        {
            acc_.impl_ptr = self;
            svc_.post(&acc_);
            svc_.work_finished();
        }
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
    class DescState,
    class ImplBase,
    class Endpoint>
native_handle_type
reactor_acceptor<Derived, Service, Op, AcceptOp, DescState, ImplBase, Endpoint>::
    do_release_socket() noexcept
{
    auto self = this->weak_from_this().lock();
    if (self)
    {
        acc_.request_cancel();

        reactor_op_base* claimed = nullptr;
        {
            std::lock_guard lock(desc_state_.mutex);
            claimed = std::exchange(desc_state_.read_op, nullptr);
            desc_state_.read_ready  = false;
            desc_state_.write_ready = false;

            if (desc_state_.is_enqueued_.load(std::memory_order_acquire))
                desc_state_.impl_ref_ = self;
        }

        if (claimed)
        {
            acc_.impl_ptr = self;
            svc_.post(&acc_);
            svc_.work_finished();
        }
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
    class DescState,
    class ImplBase,
    class Endpoint>
std::error_code
reactor_acceptor<Derived, Service, Op, AcceptOp, DescState, ImplBase, Endpoint>::
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
    class DescState,
    class ImplBase,
    class Endpoint>
std::error_code
reactor_acceptor<Derived, Service, Op, AcceptOp, DescState, ImplBase, Endpoint>::
    do_listen(int backlog)
{
    if (::listen(fd_, backlog) < 0)
        return make_err(errno);

    svc_.scheduler().register_descriptor(fd_, &desc_state_);
    return {};
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_ACCEPTOR_HPP
