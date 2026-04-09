//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_SHUTDOWN_TYPE_HPP
#define BOOST_COROSIO_SHUTDOWN_TYPE_HPP

namespace boost::corosio {

/** Different ways a socket may be shutdown.

    Used by tcp_socket, local_stream_socket, and
    local_datagram_socket to specify the direction of
    communication to disable.

    The enumerator values match the POSIX SHUT_RD / SHUT_WR /
    SHUT_RDWR convention (0, 1, 2).
*/
enum shutdown_type
{
    /// Disable further receive/read operations.
    shutdown_receive,
    /// Disable further send/write operations.
    shutdown_send,
    /// Disable both send and receive operations.
    shutdown_both
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_SHUTDOWN_TYPE_HPP
