//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_NATIVE_SOCKET_BASE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_NATIVE_SOCKET_BASE_HPP

#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/native/detail/make_err.hpp>

#include <memory>
#include <system_error>

#include <errno.h>
#include <sys/socket.h>

/*
    Readiness/completion-agnostic socket base for the POSIX-fd backends.

    Holds the part of a socket impl that does not care whether the backend
    is readiness-based (epoll/kqueue/select, which park ops on a
    descriptor_state) or completion-based (io_uring, which submits SQEs):
    the file descriptor, the cached local endpoint, the impl lifecycle
    bases (enable_shared_from_this + the service's intrusive tracking node),
    and the synchronous accessors (native_handle / options / bind).

    reactor_basic_socket derives from this and adds the readiness machinery
    (descriptor_state, register_op, the cancel/close that deregister from
    the reactor). io_uring's socket impls derive from it and add their op
    slots + SQE submission. This is the socket-layer analogue of io_uring
    deriving from reactor_scheduler: io_uring sockets share the reactor's
    readiness-agnostic socket surface, while the on-EAGAIN action (park vs
    submit-SQE) and the op model stay backend-specific.

    @tparam Derived   The concrete socket type (CRTP, for shared_from_this
                      and the intrusive node).
    @tparam ImplBase  The public vtable base (tcp_socket::implementation,
                      udp_socket::implementation, ...).
    @tparam Endpoint  The endpoint type (endpoint or local_endpoint).
*/

namespace boost::corosio::detail {

template<class Derived, class ImplBase, class Endpoint = endpoint>
class native_socket_base
    : public ImplBase
    , public std::enable_shared_from_this<Derived>
{
protected:
    // CRTP base: not publicly constructible. The check's preferred fix
    // (private ctor + `friend Derived`) is infeasible here — the reactor
    // sockets reach this base through intermediate templates
    // (reactor_stream_socket -> reactor_basic_socket) that are not `Derived`,
    // so a private ctor would stop those intermediates from constructing it.
    // Protected is the correct access; suppress the private-only suggestion.
    native_socket_base() = default;  // NOLINT(bugprone-crtp-constructor-accessibility)

    int      fd_ = -1;
    // mutable so a derived const local_endpoint() override can lazily fill
    // it via getsockname() on first read (io_uring's lazy_pending state).
    mutable Endpoint local_endpoint_;

public:
    ~native_socket_base() override = default;

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
        int level, int optname, void const* data, std::size_t size)
        noexcept override
    {
        if (::setsockopt(
                fd_, level, optname, data, static_cast<socklen_t>(size)) != 0)
            return make_err(errno);
        return {};
    }

    /// Get a socket option.
    std::error_code get_option(
        int level, int optname, void* data, std::size_t* size)
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

        Calls ::bind() and caches the resulting local endpoint via
        getsockname(). Readiness-agnostic; usable by any fd backend.

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
        socklen_t        local_len = sizeof(local_storage);
        if (::getsockname(
                fd_, reinterpret_cast<sockaddr*>(&local_storage), &local_len)
            == 0)
            local_endpoint_ =
                from_sockaddr_as(local_storage, local_len, Endpoint{});

        return {};
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_NATIVE_SOCKET_BASE_HPP
