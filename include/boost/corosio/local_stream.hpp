//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_LOCAL_STREAM_HPP
#define BOOST_COROSIO_LOCAL_STREAM_HPP

#include <boost/corosio/detail/config.hpp>

namespace boost::corosio {

class local_stream_socket;
class local_stream_acceptor;

/** Protocol tag for local (Unix domain) stream sockets.

    Identifies the local stream protocol for parameterizing
    socket and acceptor open() calls with a self-documenting
    type.

    The family(), type(), and protocol() members are implemented
    in the compiled library to avoid exposing platform socket
    headers.

    @par Example
    @code
    local_stream_socket sock(ctx);
    sock.open(local_stream{});
    @endcode

    @see native_local_stream, local_stream_socket, local_stream_acceptor
*/
class BOOST_COROSIO_DECL local_stream
{
public:
    /// Return the address family (AF_UNIX).
    static int family() noexcept;

    /// Return the socket type (SOCK_STREAM).
    static int type() noexcept;

    /// Return the protocol (0).
    static int protocol() noexcept;

    /// The associated socket type.
    using socket = local_stream_socket;

    /// The associated acceptor type.
    using acceptor = local_stream_acceptor;
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_LOCAL_STREAM_HPP
