//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_ACCEPTOR_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_ACCEPTOR_SERVICE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <system_error>

namespace boost::corosio::detail {

/** Abstract acceptor service base class.

    Concrete implementations ( epoll_acceptors, select_acceptors, etc. )
    inherit from this class and provide platform-specific acceptor
    operations. The context constructor installs whichever backend
    via `make_service`, and `tcp_acceptor.cpp` retrieves it via
    `use_service<acceptor_service>()`.
*/
class BOOST_COROSIO_DECL acceptor_service
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    /// Identifies this service for `execution_context` lookup.
    using key_type = acceptor_service;

    /** Create the acceptor socket without binding or listening.

        Creates a socket with dual-stack enabled for IPv6 but does
        not bind or listen. Does not set SO_REUSEADDR.

        @param impl The acceptor implementation to open.
        @param family Address family (e.g. `AF_INET`, `AF_INET6`).
        @param type Socket type (e.g. `SOCK_STREAM`).
        @param protocol Protocol number (e.g. `IPPROTO_TCP`).
        @return Error code on failure, empty on success.
    */
    virtual std::error_code open_acceptor_socket(
        tcp_acceptor::implementation& impl,
        int family,
        int type,
        int protocol) = 0;

    /** Bind an open acceptor to a local endpoint.

        @param impl The acceptor implementation to bind.
        @param ep The local endpoint to bind to.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code
    bind_acceptor(tcp_acceptor::implementation& impl, endpoint ep) = 0;

    /** Start listening for incoming connections.

        Registers the acceptor with the platform reactor after
        calling `::listen()`.

        @param impl The acceptor implementation to listen on.
        @param backlog The maximum length of the pending connection queue.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code
    listen_acceptor(tcp_acceptor::implementation& impl, int backlog) = 0;

protected:
    /// Construct the acceptor service.
    acceptor_service() = default;

    /// Destroy the acceptor service.
    ~acceptor_service() override = default;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_DETAIL_ACCEPTOR_SERVICE_HPP
