//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_SOCKET_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_SOCKET_SERVICE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <system_error>

namespace boost::corosio::detail {

/** Abstract socket service base class.

    Concrete implementations ( epoll_sockets, select_sockets, etc. )
    inherit from this class and provide platform-specific socket
    operations. The context constructor installs whichever backend
    via `make_service`, and `tcp_socket.cpp` retrieves it via
    `use_service<socket_service>()`.
*/
class BOOST_COROSIO_DECL socket_service
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    /// Identifies this service for `execution_context` lookup.
    using key_type = socket_service;

    /** Open a socket.

        Creates a socket and associates it with the platform reactor.

        @param impl The socket implementation to open.
        @param family Address family (e.g. `AF_INET`, `AF_INET6`).
        @param type Socket type (e.g. `SOCK_STREAM`).
        @param protocol Protocol number (e.g. `IPPROTO_TCP`).
        @return Error code on failure, empty on success.
    */
    virtual std::error_code
    open_socket( tcp_socket::implementation& impl,
                 int family, int type, int protocol ) = 0;

protected:
    /// Construct the socket service.
    socket_service() = default;

    /// Destroy the socket service.
    ~socket_service() override = default;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_DETAIL_SOCKET_SERVICE_HPP
