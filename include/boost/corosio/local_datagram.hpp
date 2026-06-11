//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_LOCAL_DATAGRAM_HPP
#define BOOST_COROSIO_LOCAL_DATAGRAM_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

namespace boost::corosio {

class local_datagram_socket;

/** Protocol tag for local (Unix domain) datagram sockets.

    Identifies the local datagram protocol for parameterizing
    socket open() calls with a self-documenting type.

    The family(), type(), and protocol() members return the
    three integers passed to the operating system's socket()
    call. Their values are platform-defined constants taken from
    the system socket headers.

    @note Not available on Windows. Windows does not support
        AF_UNIX datagram sockets (SOCK_DGRAM).

    @see local_datagram_socket
*/
class BOOST_COROSIO_DECL local_datagram
{
public:
    /// Return the address family, the platform's `AF_UNIX` constant.
    static int family() noexcept;

    /// Return the socket type, the platform's `SOCK_DGRAM` constant.
    static int type() noexcept;

    /** Return the protocol number, always `0`.

        A value of `0` directs the operating system to select the
        default protocol for an `AF_UNIX` `SOCK_DGRAM` socket. It
        is not an index into a list of protocols: Unix domain
        datagram sockets have only one, so `0` is the sole valid value.
    */
    static int protocol() noexcept;

    /// The socket type to use with this protocol, @ref local_datagram_socket.
    using socket = local_datagram_socket;
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_LOCAL_DATAGRAM_HPP
