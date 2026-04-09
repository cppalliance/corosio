//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_TCP_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_TCP_SERVICE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <system_error>

namespace boost::corosio::detail {

/** Abstract TCP service base class.

    Concrete implementations ( epoll, select, kqueue, etc. )
    inherit from this class and provide platform-specific stream
    socket operations. The context constructor installs whichever
    backend via `make_service`, and `tcp_socket.cpp` retrieves it
    via `use_service<tcp_service>()`.
*/
class BOOST_COROSIO_DECL tcp_service
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    /// Identifies this service for `execution_context` lookup.
    using key_type = tcp_service;

    /** Open a socket.

        Creates a socket and associates it with the platform reactor.

        @param impl The socket implementation to open.
        @param family Internet address family (IPv4 or IPv6).
        @param type Stream socket type.
        @param protocol TCP protocol number.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code open_socket(
        tcp_socket::implementation& impl,
        int family,
        int type,
        int protocol) = 0;

    /** Bind a stream socket to a local endpoint.

        @param impl The socket implementation to bind.
        @param ep The local endpoint to bind to.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code
    bind_socket(tcp_socket::implementation& impl, endpoint ep) = 0;

protected:
    /// Construct the TCP service.
    tcp_service() = default;

    /// Destroy the TCP service.
    ~tcp_service() override = default;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_DETAIL_TCP_SERVICE_HPP
