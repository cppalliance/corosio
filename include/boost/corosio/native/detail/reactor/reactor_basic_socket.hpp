//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_BASIC_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_BASIC_SOCKET_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/native/detail/native_socket_base.hpp>
#include <boost/corosio/native/detail/reactor/reactor_io_core.hpp>
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

/** CRTP base for reactor-backed socket implementations.

    Extracts the shared data members and virtual overrides that are
    identical across TCP (reactor_stream_socket) and UDP
    (reactor_datagram_socket). The register/park/cancel/teardown
    protocol lives in reactor_io_core.

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
    : public native_socket_base<Derived, ImplBase, Endpoint>
    , public reactor_io_core<Derived, Service, DescState>
    , public intrusive_list<Derived>::node
{
    friend Derived;

    template<class, class, class, class, class, class, class, class, class>
    friend class reactor_stream_socket;

    template<
        class,
        class,
        class,
        class,
        class,
        class,
        class,
        class,
        class,
        class,
        class>
    friend class reactor_datagram_socket;

    using core_type = reactor_io_core<Derived, Service, DescState>;

    explicit reactor_basic_socket(Service& svc) noexcept : core_type(svc) {}

protected:
    // fd_ / local_endpoint_ and the synchronous accessors (native_handle,
    // is_open, set_option/get_option, set_socket/set_local_endpoint, do_bind)
    // live in native_socket_base — the readiness/completion-agnostic base
    // shared with io_uring's sockets. The using-declarations make the
    // inherited members visible to this template's own unqualified
    // references below (two-phase lookup).
    using native_socket_base<Derived, ImplBase, Endpoint>::fd_;
    using native_socket_base<Derived, ImplBase, Endpoint>::local_endpoint_;

public:
    ~reactor_basic_socket() override = default;

    /** Assign the fd, initialize descriptor state, and register with
        the reactor.

        @param fd The descriptor to adopt.

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

    /// Cancel all pending operations.
    void do_cancel() noexcept
    {
        this->cancel_all();
    }

    /** Close the socket and cancel pending operations.

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

    /** Release the socket without closing the fd.

        Like do_close_socket() but does not call ::close().
        Returns the fd so the caller can take ownership.
    */
    native_handle_type do_release_socket() noexcept
    {
        this->abandon_all();
        native_handle_type released = fd_;
        fd_                         = -1;
        local_endpoint_             = Endpoint{};
        return released;
    }

    /** Reset descriptor state for recycling.

        Called by the owning service's `construct()` when popping this
        impl back off the free list. Actively re-initializes every
        field it owns rather than trusting `close_socket()`'s prior
        writes to still be there — `poison()` below depends on that,
        and a future field added to either side without the other
        would otherwise go unnoticed until this one re-sets it.
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
    }

#if !defined(NDEBUG)
    /** Poison the fields `reuse()` re-initializes, before the pool
        parks this impl on the free list.

        A field `reuse()` forgets to reset then reads back as this
        unmistakable byte pattern instead of silently matching
        whatever `close_socket()` happened to leave, turning a missed
        reset into an immediate, loud failure rather than a latent
        stale-state bug. See `reactor_io_core::poison_desc_state()`
        for what is left alone.

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
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_BASIC_SOCKET_HPP
