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

/* Abstract local datagram service base class.

   Concrete implementations (epoll, select, kqueue) inherit from
   this class and provide platform-specific datagram socket operations
   for Unix domain sockets. The context constructor installs
   whichever backend via make_service, and local_datagram_socket.cpp
   retrieves it via use_service<local_datagram_service>().
*/
class BOOST_COROSIO_DECL local_datagram_service
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    /// Identifies this service for execution_context lookup.
    using key_type = local_datagram_service;

    /** Open a Unix datagram socket.

        Creates a socket and associates it with the platform reactor.

        @param impl The socket implementation to open.
        @param family Unix domain address family.
        @param type Datagram socket type.
        @param protocol Protocol number (default 0).
        @return Error code on failure, empty on success.
    */
    virtual std::error_code open_socket(
        local_datagram_socket::implementation& impl,
        int family,
        int type,
        int protocol) = 0;

    /** Assign an existing file descriptor to a socket.

        Used by socketpair() to adopt pre-created fds.

        @param impl The socket implementation to assign to.
        @param fd The file descriptor to adopt.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code assign_socket(
        local_datagram_socket::implementation& impl,
        int fd) = 0;

    /** Bind a datagram socket to a local endpoint.

        @param impl The socket implementation to bind.
        @param ep The local endpoint to bind to.
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
