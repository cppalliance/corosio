//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_UDP_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_UDP_SERVICE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <system_error>

namespace boost::corosio::detail {

/** Abstract UDP service base class.

    Concrete implementations (epoll_udp_service,
    select_udp_service, etc.) inherit from this class and
    provide platform-specific datagram socket operations. The
    context constructor installs whichever backend via
    `make_service`, and `udp_socket.cpp` retrieves it via
    `use_service<udp_service>()`.
*/
class BOOST_COROSIO_DECL udp_service
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    /// Identifies this service for `execution_context` lookup.
    using key_type = udp_service;

    /** Open a datagram socket.

        Creates a socket and associates it with the platform reactor.

        @param impl The socket implementation to open.
        @param family Internet address family (IPv4 or IPv6).
        @param type Datagram socket type.
        @param protocol UDP protocol number.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code open_datagram_socket(
        udp_socket::implementation& impl,
        int family,
        int type,
        int protocol) = 0;

    /** Bind a datagram socket to a local endpoint.

        @param impl The socket implementation to bind.
        @param ep The local endpoint to bind to.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code
    bind_datagram(udp_socket::implementation& impl, endpoint ep) = 0;

protected:
    /// Construct the UDP service.
    udp_service() = default;

    /// Destroy the UDP service.
    ~udp_service() override = default;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_DETAIL_UDP_SERVICE_HPP
