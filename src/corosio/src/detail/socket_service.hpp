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
#include <boost/corosio/socket.hpp>
#include <boost/corosio/acceptor.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/system/error_code.hpp>

/*
    Abstract Socket Service
    =======================

    These abstract base classes enable runtime backend selection for socket
    and acceptor operations. Both epoll_sockets and select_sockets derive
    from socket_service and use it as their key_type. This allows
    use_service<socket_service>() to return whichever implementation was
    installed first (by the context constructor).

    Design Pattern:
    - socket_service is the abstract base with key_type = socket_service
    - Concrete implementations (epoll_sockets, select_sockets) inherit from it
    - The concrete implementation's key_type is inherited from socket_service
    - Whichever context type is constructed first installs its implementation
    - socket.cpp and acceptor.cpp use the abstract interface

    This enables:
    - epoll_context installs epoll_sockets via make_service<epoll_sockets>()
    - select_context installs select_sockets via make_service<select_sockets>()
    - socket.cpp uses use_service<socket_service>() to get whichever is installed
*/

namespace boost::corosio::detail {

//------------------------------------------------------------------------------

/** Abstract socket service base class.

    This is the service interface used by socket.cpp. Concrete implementations
    (epoll_socket_service, select_socket_service, etc.) inherit from this class
    and provide the actual socket operations.

    The key_type is socket_service itself, which enables runtime polymorphism:
    whichever concrete implementation is installed first by a context constructor
    will be returned by find_service<socket_service>().
*/
class socket_service : public capy::execution_context::service
{
public:
    using key_type = socket_service;

    /** Create a new socket implementation.

        @return Reference to the newly created socket implementation.
    */
    virtual socket::socket_impl& create_impl() = 0;

    /** Destroy a socket implementation.

        @param impl The socket implementation to destroy.
    */
    virtual void destroy_impl(socket::socket_impl& impl) = 0;

    /** Open a socket.

        Creates an IPv4 TCP socket and associates it with the platform reactor.

        @param impl The socket implementation to open.
        @return Error code on failure, empty on success.
    */
    virtual system::error_code open_socket(socket::socket_impl& impl) = 0;

protected:
    socket_service() = default;
    ~socket_service() override = default;
};

//------------------------------------------------------------------------------

/** Abstract acceptor service base class.

    This is the service interface used by acceptor.cpp. Concrete implementations
    (epoll_acceptor_service, select_acceptor_service, etc.) inherit from this class
    and provide the actual acceptor operations.

    The key_type is acceptor_service itself, which enables runtime polymorphism.
*/
class acceptor_service : public capy::execution_context::service
{
public:
    using key_type = acceptor_service;

    /** Create a new acceptor implementation.

        @return Reference to the newly created acceptor implementation.
    */
    virtual acceptor::acceptor_impl& create_acceptor_impl() = 0;

    /** Destroy an acceptor implementation.

        @param impl The acceptor implementation to destroy.
    */
    virtual void destroy_acceptor_impl(acceptor::acceptor_impl& impl) = 0;

    /** Open an acceptor.

        Creates an IPv4 TCP socket, binds it to the specified endpoint,
        and begins listening for incoming connections.

        @param impl The acceptor implementation to open.
        @param ep The local endpoint to bind to.
        @param backlog The maximum length of the queue of pending connections.
        @return Error code on failure, empty on success.
    */
    virtual system::error_code open_acceptor(
        acceptor::acceptor_impl& impl,
        endpoint ep,
        int backlog) = 0;

protected:
    acceptor_service() = default;
    ~acceptor_service() override = default;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_DETAIL_SOCKET_SERVICE_HPP
