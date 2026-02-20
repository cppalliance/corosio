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

    /** Open an acceptor.

        Creates an IPv4 TCP socket, binds it to the specified endpoint,
        and begins listening for incoming connections.

        @param impl The acceptor implementation to open.
        @param ep The local endpoint to bind to.
        @param backlog The maximum length of the queue of pending connections.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code open_acceptor(
        tcp_acceptor::implementation& impl, endpoint ep, int backlog) = 0;

protected:
    /// Construct the acceptor service.
    acceptor_service() = default;

    /// Destroy the acceptor service.
    ~acceptor_service() override = default;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_DETAIL_ACCEPTOR_SERVICE_HPP
