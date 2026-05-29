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

    The family(), type(), and protocol() members are implemented
    in the compiled library to avoid exposing platform socket
    headers.

    @note Not available on Windows. Windows does not support
        AF_UNIX datagram sockets (SOCK_DGRAM).

    @see local_datagram_socket
*/
class BOOST_COROSIO_DECL local_datagram
{
public:
    /// Return the address family (AF_UNIX).
    static int family() noexcept;

    /// Return the socket type (SOCK_DGRAM).
    static int type() noexcept;

    /// Return the protocol (0).
    static int protocol() noexcept;

    /// The associated socket type.
    using socket = local_datagram_socket;
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_LOCAL_DATAGRAM_HPP
