//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_LOCAL_SOCKET_PAIR_HPP
#define BOOST_COROSIO_LOCAL_SOCKET_PAIR_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/local_stream_socket.hpp>

#if BOOST_COROSIO_POSIX
#include <boost/corosio/local_datagram_socket.hpp>
#endif

#include <utility>

namespace boost::corosio {

class io_context;

/** Create a connected pair of local stream sockets.

    On POSIX, uses socketpair(AF_UNIX, SOCK_STREAM). On Windows,
    emulates socketpair by creating a temporary listener, connecting,
    and accepting. Data written to one socket can be read from the
    other.

    @param ctx The I/O context for the sockets.

    @return A pair of connected local stream sockets.

    @throws std::system_error on failure.
*/
BOOST_COROSIO_DECL std::pair<local_stream_socket, local_stream_socket>
make_local_stream_pair(io_context& ctx);

#if BOOST_COROSIO_POSIX
/** Create a connected pair of local datagram sockets.

    Uses socketpair(AF_UNIX, SOCK_DGRAM) to create two
    pre-connected sockets.

    @note Not available on Windows. Windows does not support
        AF_UNIX datagram sockets.

    @param ctx The I/O context for the sockets.

    @return A pair of connected local datagram sockets.

    @throws std::system_error on failure.
*/
BOOST_COROSIO_DECL std::pair<local_datagram_socket, local_datagram_socket>
make_local_datagram_pair(io_context& ctx);
#endif

} // namespace boost::corosio

#endif // BOOST_COROSIO_LOCAL_SOCKET_PAIR_HPP
