//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_LOCAL_CONNECT_PAIR_HPP
#define BOOST_COROSIO_LOCAL_CONNECT_PAIR_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/local_stream_socket.hpp>

#if BOOST_COROSIO_POSIX
#include <boost/corosio/local_datagram_socket.hpp>
#endif

#include <system_error>

namespace boost::corosio {

/** Synchronously connect two AF_UNIX stream sockets as a connected pair.

    On POSIX the implementation uses `socketpair(AF_UNIX, SOCK_STREAM)`
    and adopts the descriptors via `assign()`. On Windows it performs a
    private bind/listen/accept on the calling thread paired with a
    `connect()` on a short-lived worker thread; the caller's
    `io_context` is never driven, so it may be running on another
    thread.

    Either socket may be a `native_local_stream_socket<Backend>`; the
    base reference selects the backend's `assign_socket` through normal
    virtual dispatch.

    @par Preconditions
    Both sockets must be in the closed state.

    @par Exception Safety
    Nothrow. On failure both sockets remain closed and any underlying
    resources are released.

    @param a Receives the accepted/first endpoint of the pair.
    @param b Receives the connected/second endpoint of the pair.

    @return Empty on success; otherwise the underlying system error.
*/
BOOST_COROSIO_DECL
std::error_code
connect_pair(local_stream_socket& a, local_stream_socket& b) noexcept;

#if BOOST_COROSIO_POSIX

/** Synchronously connect two AF_UNIX datagram sockets as a connected pair.

    POSIX only. Uses `socketpair(AF_UNIX, SOCK_DGRAM)` and adopts the
    descriptors via `assign()`.

    @par Preconditions
    Both sockets must be in the closed state.

    @par Exception Safety
    Nothrow.

    @param a First socket of the pair.
    @param b Second socket of the pair.

    @return Empty on success; otherwise the underlying system error.
*/
BOOST_COROSIO_DECL
std::error_code
connect_pair(local_datagram_socket& a, local_datagram_socket& b) noexcept;

#endif // BOOST_COROSIO_POSIX

} // namespace boost::corosio

#endif
