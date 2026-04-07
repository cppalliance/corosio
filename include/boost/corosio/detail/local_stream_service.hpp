//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_LOCAL_STREAM_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_LOCAL_STREAM_SERVICE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <system_error>

namespace boost::corosio::detail {

/* Abstract local stream service base class.

   Concrete implementations (epoll, select, kqueue) inherit from
   this class and provide platform-specific stream socket operations
   for Unix domain sockets. The context constructor installs
   whichever backend via make_service, and local_stream_socket.cpp
   retrieves it via use_service<local_stream_service>().
*/
class BOOST_COROSIO_DECL local_stream_service
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    /// Identifies this service for execution_context lookup.
    using key_type = local_stream_service;

    /** Open a local (Unix domain) stream socket.

        Creates a socket and associates it with the platform
        I/O backend (reactor or IOCP).

        @param impl The socket implementation to open.
        @param family Address family for local IPC.
        @param type Socket type for stream sockets.
        @param protocol Protocol number (typically 0).
        @return Error code on failure, empty on success.
    */
    virtual std::error_code open_socket(
        local_stream_socket::implementation& impl,
        int family,
        int type,
        int protocol) = 0;

    /** Assign an existing native socket handle to a socket.

        Adopts a pre-created socket handle (e.g. from a
        platform-specific pair creation API). On success the
        impl takes ownership and will close the handle. On
        failure the caller retains ownership and must close
        it. On platforms that do not support handle adoption,
        returns @c operation_not_supported.

        @param impl The socket implementation to assign to.
        @param fd The native socket handle to adopt.
        @return Error code on failure, empty on success.
    */
    virtual std::error_code assign_socket(
        local_stream_socket::implementation& impl,
        native_handle_type fd) = 0;

protected:
    local_stream_service() = default;
    ~local_stream_service() override = default;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_DETAIL_LOCAL_STREAM_SERVICE_HPP
