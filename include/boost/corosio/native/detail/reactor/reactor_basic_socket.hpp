//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_BASIC_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_BASIC_SOCKET_HPP

#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/endpoint.hpp>
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

/** CRTP base for reactor-backed socket implementations.

    Extracts the shared data members, virtual overrides, and
    cancel/close/register logic that is identical across TCP
    (reactor_stream_socket) and UDP (reactor_datagram_socket).

    Derived classes provide CRTP callbacks that enumerate their
    specific op slots so cancel/close can iterate them generically.

    @tparam Derived   The concrete socket type (CRTP).
    @tparam ImplBase  The public vtable base (tcp_socket::implementation
                      or udp_socket::implementation).
    @tparam Service   The backend's service type.
    @tparam DescState The backend's descriptor_state type.
    @tparam Endpoint  The endpoint type (endpoint or local_endpoint).
*/
template<
    class Derived,
    class ImplBase,
    class Service,
    class DescState,
    class Endpoint = endpoint>
class reactor_basic_socket
    : public ImplBase
    , public std::enable_shared_from_this<Derived>
    , public intrusive_list<Derived>::node
{
    friend Derived;

    template<class, class, class, class, class, class, class, class, class>
    friend class reactor_stream_socket;

    template<class, class, class, class, class, class, class, class, class, class, class>
    friend class reactor_datagram_socket;

    explicit reactor_basic_socket(Service& svc) noexcept : svc_(svc) {}

protected:
    Service& svc_;
    int fd_ = -1;
    Endpoint local_endpoint_;

public:
    /// Per-descriptor state for persistent reactor registration.
    DescState desc_state_;

    ~reactor_basic_socket() override = default;

    /// Return the underlying file descriptor.
    native_handle_type native_handle() const noexcept override
    {
        return fd_;
    }

    /// Return the cached local endpoint.
    Endpoint local_endpoint() const noexcept override
    {
        return local_endpoint_;
    }

    /// Return true if the socket has an open file descriptor.
    bool is_open() const noexcept
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

    /// Assign the file descriptor.
    void set_socket(int fd) noexcept
    {
        fd_ = fd;
    }

    /// Cache the local endpoint.
    void set_local_endpoint(Endpoint ep) noexcept
    {
        local_endpoint_ = ep;
    }

    /** Bind the socket to a local endpoint.

        Calls ::bind() and caches the resulting local endpoint
        via getsockname().

        @param ep The endpoint to bind to.
        @return Error code on failure, empty on success.
    */
    std::error_code do_bind(Endpoint const& ep) noexcept
    {
        sockaddr_storage storage{};
        socklen_t addrlen = to_sockaddr(ep, socket_family(fd_), storage);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&storage), addrlen) != 0)
            return make_err(errno);

        sockaddr_storage local_storage{};
        socklen_t local_len = sizeof(local_storage);
        if (::getsockname(
                fd_, reinterpret_cast<sockaddr*>(&local_storage), &local_len) ==
            0)
            local_endpoint_ =
                from_sockaddr_as(local_storage, local_len, Endpoint{});

        return {};
    }

    /// Assign the fd, initialize descriptor state, and register with the reactor.
    void init_and_register(int fd) noexcept
    {
        fd_ = fd;
        desc_state_.fd = fd;
        {
            std::lock_guard lock(desc_state_.mutex);
            desc_state_.read_op    = nullptr;
            desc_state_.write_op   = nullptr;
            desc_state_.connect_op = nullptr;
        }
        svc_.scheduler().register_descriptor(fd, &desc_state_);
    }

    /** Register an op with the reactor.

        Handles cached edge events and deferred cancellation.
        Called on the EAGAIN/EINPROGRESS path when speculative
        I/O failed.
    */
    template<class Op>
    void register_op(
        Op& op,
        reactor_op_base*& desc_slot,
        bool& ready_flag,
        bool& cancel_flag,
        bool is_write_direction = false) noexcept;

    /** Cancel a single pending operation.

        Claims the operation from its descriptor_state slot under
        the mutex and posts it to the scheduler as cancelled.
        Derived must implement:
          op_to_desc_slot(Op&) -> reactor_op_base**
          op_to_cancel_flag(Op&) -> bool*
    */
    template<class Op>
    void cancel_single_op(Op& op) noexcept;

    /** Cancel all pending operations.

        Invoked by the derived class's cancel() override.
        Derived must implement:
          for_each_op(auto fn)
          for_each_desc_entry(auto fn)
    */
    void do_cancel() noexcept;

    /** Close the socket and cancel pending operations.

        Invoked by the derived class's close_socket(). The
        derived class may add backend-specific cleanup after
        calling this method.
        Derived must implement:
          for_each_op(auto fn)
          for_each_desc_entry(auto fn)
    */
    void do_close_socket() noexcept;

    /** Release the socket without closing the fd.

        Like do_close_socket() but does not call ::close().
        Returns the fd so the caller can take ownership.
    */
    native_handle_type do_release_socket() noexcept;
};

template<class Derived, class ImplBase, class Service, class DescState, class Endpoint>
template<class Op>
void
reactor_basic_socket<Derived, ImplBase, Service, DescState, Endpoint>::register_op(
    Op& op,
    reactor_op_base*& desc_slot,
    bool& ready_flag,
    bool& cancel_flag,
    bool is_write_direction) noexcept
{
    svc_.work_started();

    std::lock_guard lock(desc_state_.mutex);
    bool io_done = false;
    if (ready_flag)
    {
        ready_flag = false;
        op.perform_io();
        io_done = (op.errn != EAGAIN && op.errn != EWOULDBLOCK);
        if (!io_done)
            op.errn = 0;
    }

    if (cancel_flag)
    {
        cancel_flag = false;
        op.cancelled.store(true, std::memory_order_relaxed);
    }

    if (io_done || op.cancelled.load(std::memory_order_acquire))
    {
        svc_.post(&op);
        svc_.work_finished();
    }
    else
    {
        desc_slot = &op;

        // Select must rebuild its fd_sets when a write-direction op
        // is parked, so select() watches for writability. Compiled
        // away to nothing for epoll and kqueue.
        if constexpr (requires { Service::needs_write_notification; })
        {
            if constexpr (Service::needs_write_notification)
            {
                if (is_write_direction)
                    svc_.scheduler().notify_reactor();
            }
        }
    }
}

template<class Derived, class ImplBase, class Service, class DescState, class Endpoint>
template<class Op>
void
reactor_basic_socket<Derived, ImplBase, Service, DescState, Endpoint>::cancel_single_op(
    Op& op) noexcept
{
    auto self = this->weak_from_this().lock();
    if (!self)
        return;

    op.request_cancel();

    auto* d                       = static_cast<Derived*>(this);
    reactor_op_base** desc_op_ptr = d->op_to_desc_slot(op);

    if (desc_op_ptr)
    {
        reactor_op_base* claimed = nullptr;
        {
            std::lock_guard lock(desc_state_.mutex);
            if (*desc_op_ptr == &op)
                claimed = std::exchange(*desc_op_ptr, nullptr);
            else
            {
                bool* cflag = d->op_to_cancel_flag(op);
                if (cflag)
                    *cflag = true;
            }
        }
        if (claimed)
        {
            op.impl_ptr = self;
            svc_.post(&op);
            svc_.work_finished();
        }
    }
}

template<class Derived, class ImplBase, class Service, class DescState, class Endpoint>
void
reactor_basic_socket<Derived, ImplBase, Service, DescState, Endpoint>::
    do_cancel() noexcept
{
    auto self = this->weak_from_this().lock();
    if (!self)
        return;

    auto* d = static_cast<Derived*>(this);

    d->for_each_op([](auto& op) { op.request_cancel(); });

    // Claim ops under a single lock acquisition
    struct claimed_entry
    {
        reactor_op_base* op   = nullptr;
        reactor_op_base* base = nullptr;
    };
    // Max 8 ops: conn, rd, wr, wait_rd, wait_wr, wait_er, recv_rd, send_wr
    claimed_entry claimed[8];
    int count = 0;

    {
        std::lock_guard lock(desc_state_.mutex);
        d->for_each_desc_entry([&](auto& op, reactor_op_base*& desc_slot) {
            if (desc_slot == &op)
            {
                claimed[count].op   = std::exchange(desc_slot, nullptr);
                claimed[count].base = &op;
                ++count;
            }
        });
    }

    for (int i = 0; i < count; ++i)
    {
        claimed[i].base->impl_ptr = self;
        svc_.post(claimed[i].base);
        svc_.work_finished();
    }
}

template<class Derived, class ImplBase, class Service, class DescState, class Endpoint>
void
reactor_basic_socket<Derived, ImplBase, Service, DescState, Endpoint>::
    do_close_socket() noexcept
{
    auto self = this->weak_from_this().lock();
    if (self)
    {
        auto* d = static_cast<Derived*>(this);

        d->for_each_op([](auto& op) { op.request_cancel(); });

        struct claimed_entry
        {
            reactor_op_base* base = nullptr;
        };
        claimed_entry claimed[8];
        int count = 0;

        {
            std::lock_guard lock(desc_state_.mutex);
            d->for_each_desc_entry(
                [&](auto& /*op*/, reactor_op_base*& desc_slot) {
                    auto* c = std::exchange(desc_slot, nullptr);
                    if (c)
                    {
                        claimed[count].base = c;
                        ++count;
                    }
                });
            desc_state_.read_ready             = false;
            desc_state_.write_ready            = false;
            desc_state_.read_cancel_pending       = false;
            desc_state_.write_cancel_pending      = false;
            desc_state_.connect_cancel_pending    = false;
            desc_state_.wait_read_cancel_pending  = false;
            desc_state_.wait_write_cancel_pending = false;
            desc_state_.wait_error_cancel_pending = false;

            if (desc_state_.is_enqueued_.load(std::memory_order_acquire))
                desc_state_.impl_ref_ = self;
        }

        for (int i = 0; i < count; ++i)
        {
            claimed[i].base->impl_ptr = self;
            svc_.post(claimed[i].base);
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

template<class Derived, class ImplBase, class Service, class DescState, class Endpoint>
native_handle_type
reactor_basic_socket<Derived, ImplBase, Service, DescState, Endpoint>::
    do_release_socket() noexcept
{
    // Cancel pending ops (same as do_close_socket)
    auto self = this->weak_from_this().lock();
    if (self)
    {
        auto* d = static_cast<Derived*>(this);

        d->for_each_op([](auto& op) { op.request_cancel(); });

        struct claimed_entry
        {
            reactor_op_base* base = nullptr;
        };
        claimed_entry claimed[8];
        int count = 0;

        {
            std::lock_guard lock(desc_state_.mutex);
            d->for_each_desc_entry(
                [&](auto& /*op*/, reactor_op_base*& desc_slot) {
                    auto* c = std::exchange(desc_slot, nullptr);
                    if (c)
                    {
                        claimed[count].base = c;
                        ++count;
                    }
                });
            desc_state_.read_ready             = false;
            desc_state_.write_ready            = false;
            desc_state_.read_cancel_pending       = false;
            desc_state_.write_cancel_pending      = false;
            desc_state_.connect_cancel_pending    = false;
            desc_state_.wait_read_cancel_pending  = false;
            desc_state_.wait_write_cancel_pending = false;
            desc_state_.wait_error_cancel_pending = false;

            if (desc_state_.is_enqueued_.load(std::memory_order_acquire))
                desc_state_.impl_ref_ = self;
        }

        for (int i = 0; i < count; ++i)
        {
            claimed[i].base->impl_ptr = self;
            svc_.post(claimed[i].base);
            svc_.work_finished();
        }
    }

    native_handle_type released = fd_;

    if (fd_ >= 0)
    {
        if (desc_state_.registered_events != 0)
            svc_.scheduler().deregister_descriptor(fd_);
        // Do NOT close -- caller takes ownership
        fd_ = -1;
    }

    desc_state_.fd                = -1;
    desc_state_.registered_events = 0;

    local_endpoint_ = Endpoint{};

    return released;
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_BASIC_SOCKET_HPP
