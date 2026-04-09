//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_LOCAL_STREAM_ACCEPTOR_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_LOCAL_STREAM_ACCEPTOR_SERVICE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <system_error>

namespace boost::corosio::detail {

/* Abstract local stream acceptor service base class.

   Concrete implementations (epoll, select, kqueue) inherit
   from this class and provide platform-specific acceptor
   operations for Unix domain sockets.
*/
class BOOST_COROSIO_DECL local_stream_acceptor_service
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    /// Identifies this service for execution_context lookup.
    using key_type = local_stream_acceptor_service;

    /** Create the acceptor socket.

        @param impl The acceptor implementation to open.
        @param family Unix domain address family.
        @param type Stream socket type.
        @param protocol Protocol number (default 0).
        @return Error code on failure, empty on success.
    */
    virtual std::error_code open_acceptor_socket(
        local_stream_acceptor::implementation& impl,
        int family,
        int type,
        int protocol) = 0;

    /** Bind an open acceptor to a local endpoint.

        @param impl The acceptor implementation to bind.
        @param ep The local endpoint (path) to bind to.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code bind_acceptor(
        local_stream_acceptor::implementation& impl,
        local_endpoint ep) = 0;

    /** Start listening for incoming connections.

        @param impl The acceptor implementation to listen on.
        @param backlog The maximum pending connection queue length.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code listen_acceptor(
        local_stream_acceptor::implementation& impl,
        int backlog) = 0;

protected:
    local_stream_acceptor_service() = default;
    ~local_stream_acceptor_service() override = default;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_DETAIL_LOCAL_STREAM_ACCEPTOR_SERVICE_HPP
