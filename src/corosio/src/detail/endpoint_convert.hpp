//
// Copyright (c) 2026 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_ENDPOINT_CONVERT_HPP
#define BOOST_COROSIO_DETAIL_ENDPOINT_CONVERT_HPP

#include <boost/corosio/endpoint.hpp>

#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <Ws2tcpip.h>
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace boost {
namespace corosio {
namespace detail {

/** Convert IPv4 endpoint to sockaddr_in.

    @param ep The endpoint to convert. Must be IPv4 (is_v4() == true).
    @return A sockaddr_in structure with fields in network byte order.
*/
inline
sockaddr_in
to_sockaddr_in(endpoint const& ep) noexcept
{
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(ep.port());
    auto bytes = ep.v4_address().to_bytes();
    std::memcpy(&sa.sin_addr, bytes.data(), 4);
    return sa;
}

/** Convert IPv6 endpoint to sockaddr_in6.

    @param ep The endpoint to convert. Must be IPv6 (is_v6() == true).
    @return A sockaddr_in6 structure with fields in network byte order.
*/
inline
sockaddr_in6
to_sockaddr_in6(endpoint const& ep) noexcept
{
    sockaddr_in6 sa{};
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(ep.port());
    auto bytes = ep.v6_address().to_bytes();
    std::memcpy(&sa.sin6_addr, bytes.data(), 16);
    return sa;
}

/** Create endpoint from sockaddr_in.

    @param sa The sockaddr_in structure with fields in network byte order.
    @return An endpoint with address and port extracted from sa.
*/
inline
endpoint
from_sockaddr_in(sockaddr_in const& sa) noexcept
{
    urls::ipv4_address::bytes_type bytes;
    std::memcpy(bytes.data(), &sa.sin_addr, 4);
    return endpoint(urls::ipv4_address(bytes), ntohs(sa.sin_port));
}

/** Create endpoint from sockaddr_in6.

    @param sa The sockaddr_in6 structure with fields in network byte order.
    @return An endpoint with address and port extracted from sa.
*/
inline
endpoint
from_sockaddr_in6(sockaddr_in6 const& sa) noexcept
{
    urls::ipv6_address::bytes_type bytes;
    std::memcpy(bytes.data(), &sa.sin6_addr, 16);
    return endpoint(urls::ipv6_address(bytes), ntohs(sa.sin6_port));
}

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
