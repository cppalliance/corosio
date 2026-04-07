//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_LOCAL_DATAGRAM_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_LOCAL_DATAGRAM_SERVICE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <system_error>

namespace boost::corosio::detail {

/** Abstract local datagram service base class.

    Concrete implementations (epoll, select, kqueue, IOCP)
    inherit from this class and provide platform-specific
    datagram socket operations for local (Unix domain) sockets.

    Instances are looked up via key_type in the
    execution_context. All errors are reported via the returned
    std::error_code; these methods do not throw.
*/
class BOOST_COROSIO_DECL local_datagram_service
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    /// Identifies this service for execution_context lookup.
    using key_type = local_datagram_service;

    /** Open a local (Unix domain) datagram socket.

        Creates a socket and associates it with the platform
        I/O backend (reactor or IOCP).

        @param impl The socket implementation to open.
            Must not already represent an open socket.
        @param family Address family for local IPC.
        @param type Socket type for datagram sockets.
        @param protocol Protocol number (typically 0).
        @return Error code on failure, empty on success.
    */
    virtual std::error_code open_socket(
        local_datagram_socket::implementation& impl,
        int family,
        int type,
        int protocol) = 0;

    /** Assign an existing native socket handle to a socket.

        Adopts a pre-created socket handle. On success the
        impl takes ownership and will close the handle. On
        failure the caller retains ownership and must close
        it. On platforms that do not support handle adoption,
        returns @c operation_not_supported.

        @param impl The socket implementation to assign to.
        @param fd The native socket handle to adopt.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code assign_socket(
        local_datagram_socket::implementation& impl,
        native_handle_type fd) = 0;

    /** Bind a datagram socket to a local endpoint.

        @pre @p impl represents an open socket (via
            open_socket() or assign_socket()).
        @param impl The socket implementation to bind.
        @param ep The local endpoint to bind to.
            Copied; need not remain valid after the call.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code bind_socket(
        local_datagram_socket::implementation& impl,
        corosio::local_endpoint ep) = 0;

protected:
    local_datagram_service() = default;
    ~local_datagram_service() override = default;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_DETAIL_LOCAL_DATAGRAM_SERVICE_HPP
