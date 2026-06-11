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

    The family(), type(), and protocol() members return the
    three integers passed to the operating system's socket()
    call. Their values are platform-defined constants taken from
    the system socket headers.

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
    /// Return the address family, the platform's `AF_UNIX` constant.
    static int family() noexcept;

    /// Return the socket type, the platform's `SOCK_STREAM` constant.
    static int type() noexcept;

    /** Return the protocol number, always `0`.

        A value of `0` directs the operating system to select the
        default protocol for an `AF_UNIX` `SOCK_STREAM` socket. It
        is not an index into a list of protocols: Unix domain
        stream sockets have only one, so `0` is the sole valid value.
    */
    static int protocol() noexcept;

    /// The socket type to use with this protocol, @ref local_stream_socket.
    using socket = local_stream_socket;

    /// The acceptor type to use with this protocol, @ref local_stream_acceptor.
    using acceptor = local_stream_acceptor;
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_LOCAL_STREAM_HPP
