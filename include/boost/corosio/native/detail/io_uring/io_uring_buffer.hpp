//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_BUFFER_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_BUFFER_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IO_URING

#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/native/detail/endpoint_convert.hpp>

namespace boost::corosio::detail {

/** Convert a corosio::endpoint to a sockaddr_storage.

    Fills `out` with the appropriate sockaddr_in (IPv4) or sockaddr_in6
    (IPv6) representation, with all fields in network byte order.

    @param ep  The endpoint to convert.
    @param out Destination storage; zeroed then written.
    @return    The actual address length written into `out`
               (`sizeof(sockaddr_in)` or `sizeof(sockaddr_in6)`).
*/
inline socklen_t
endpoint_to_sockaddr(endpoint const& ep, sockaddr_storage& out) noexcept
{
    return to_sockaddr(ep, out);
}

/// Convert a corosio::local_endpoint to a sockaddr_storage.
inline socklen_t
endpoint_to_sockaddr(corosio::local_endpoint const& ep, sockaddr_storage& out) noexcept
{
    return to_sockaddr(ep, out);
}

/** Convert a sockaddr_storage to a corosio::endpoint.

    Dispatches on `sa.ss_family`; returns a default-constructed
    endpoint for any family other than `AF_INET` or `AF_INET6`.

    @param sa The sockaddr_storage in network byte order.
    @return   The reconstructed endpoint.
*/
inline endpoint
sockaddr_to_endpoint(sockaddr_storage const& sa) noexcept
{
    return from_sockaddr(sa);
}

/// Convert a sockaddr_storage to a corosio::local_endpoint.
inline corosio::local_endpoint
sockaddr_to_local_endpoint(
    sockaddr_storage const& sa, socklen_t len) noexcept
{
    return from_sockaddr_local(sa, len);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IO_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_BUFFER_HPP
