//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_SOCKET_PAIR_HPP
#define BOOST_COROSIO_TEST_SOCKET_PAIR_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/socket.hpp>

#include <utility>

namespace boost::corosio::test {

/** Create a connected pair of sockets.

    Creates two sockets connected via loopback TCP sockets.
    Data written to one socket can be read from the other.

    @param ioc The io_context for the sockets.

    @return A pair of connected sockets.
*/
BOOST_COROSIO_DECL
std::pair<socket, socket>
make_socket_pair(io_context& ioc);

} // namespace boost::corosio::test

#endif
