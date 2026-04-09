//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_ENDPOINT_CONVERT_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_ENDPOINT_CONVERT_HPP

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/detail/platform.hpp>

#include <cstring>

#if BOOST_COROSIO_POSIX
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <Ws2tcpip.h>
#endif

namespace boost::corosio::detail {

/** Convert IPv4 endpoint to sockaddr_in.

    @param ep The endpoint to convert. Must be IPv4 (is_v4() == true).
    @return A sockaddr_in structure with fields in network byte order.
*/
inline sockaddr_in
to_sockaddr_in(endpoint const& ep) noexcept
{
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(ep.port());
    auto bytes    = ep.v4_address().to_bytes();
    std::memcpy(&sa.sin_addr, bytes.data(), 4);
    return sa;
}

/** Convert IPv6 endpoint to sockaddr_in6.

    @param ep The endpoint to convert. Must be IPv6 (is_v6() == true).
    @return A sockaddr_in6 structure with fields in network byte order.
*/
inline sockaddr_in6
to_sockaddr_in6(endpoint const& ep) noexcept
{
    sockaddr_in6 sa{};
    sa.sin6_family = AF_INET6;
    sa.sin6_port   = htons(ep.port());
    auto bytes     = ep.v6_address().to_bytes();
    std::memcpy(&sa.sin6_addr, bytes.data(), 16);
    return sa;
}

/** Create endpoint from sockaddr_in.

    @param sa The sockaddr_in structure with fields in network byte order.
    @return An endpoint with address and port extracted from sa.
*/
inline endpoint
from_sockaddr_in(sockaddr_in const& sa) noexcept
{
    ipv4_address::bytes_type bytes;
    std::memcpy(bytes.data(), &sa.sin_addr, 4);
    return endpoint(ipv4_address(bytes), ntohs(sa.sin_port));
}

/** Create endpoint from sockaddr_in6.

    @param sa The sockaddr_in6 structure with fields in network byte order.
    @return An endpoint with address and port extracted from sa.
*/
inline endpoint
from_sockaddr_in6(sockaddr_in6 const& sa) noexcept
{
    ipv6_address::bytes_type bytes;
    std::memcpy(bytes.data(), &sa.sin6_addr, 16);
    return endpoint(ipv6_address(bytes), ntohs(sa.sin6_port));
}

/** Convert an IPv4 endpoint to an IPv4-mapped IPv6 sockaddr_in6.

    Produces a `sockaddr_in6` with the `::ffff:` prefix, suitable
    for passing an IPv4 destination to a dual-stack IPv6 socket.

    @param ep The endpoint to convert. Must be IPv4 (is_v4() == true).
    @return A sockaddr_in6 with the IPv4-mapped address.
*/
inline sockaddr_in6
to_v4_mapped_sockaddr_in6(endpoint const& ep) noexcept
{
    sockaddr_in6 sa{};
    sa.sin6_family = AF_INET6;
    sa.sin6_port   = htons(ep.port());
    // ::ffff:0:0/96 prefix
    sa.sin6_addr.s6_addr[10] = 0xff;
    sa.sin6_addr.s6_addr[11] = 0xff;
    auto bytes               = ep.v4_address().to_bytes();
    std::memcpy(&sa.sin6_addr.s6_addr[12], bytes.data(), 4);
    return sa;
}

/** Convert endpoint to sockaddr_storage.

    Dispatches to @ref to_sockaddr_in or @ref to_sockaddr_in6
    based on the endpoint's address family.

    @param ep The endpoint to convert.
    @param storage Output parameter filled with the sockaddr.
    @return The length of the filled sockaddr structure.
*/
inline socklen_t
to_sockaddr(endpoint const& ep, sockaddr_storage& storage) noexcept
{
    std::memset(&storage, 0, sizeof(storage));
    if (ep.is_v4())
    {
        auto sa = to_sockaddr_in(ep);
        std::memcpy(&storage, &sa, sizeof(sa));
        return sizeof(sa);
    }
    auto sa6 = to_sockaddr_in6(ep);
    std::memcpy(&storage, &sa6, sizeof(sa6));
    return sizeof(sa6);
}

/** Convert endpoint to sockaddr_storage for a specific socket family.

    When the socket is AF_INET6 and the endpoint is IPv4, the address
    is converted to an IPv4-mapped IPv6 address (`::ffff:x.x.x.x`) so
    dual-stack sockets can connect to IPv4 destinations.

    @param ep The endpoint to convert.
    @param socket_family The address family of the socket (AF_INET or
        AF_INET6).
    @param storage Output parameter filled with the sockaddr.
    @return The length of the filled sockaddr structure.
*/
inline socklen_t
to_sockaddr(
    endpoint const& ep, int socket_family, sockaddr_storage& storage) noexcept
{
    // IPv4 endpoint on IPv6 socket: use IPv4-mapped address
    if (ep.is_v4() && socket_family == AF_INET6)
    {
        std::memset(&storage, 0, sizeof(storage));
        auto sa6 = to_v4_mapped_sockaddr_in6(ep);
        std::memcpy(&storage, &sa6, sizeof(sa6));
        return sizeof(sa6);
    }
    return to_sockaddr(ep, storage);
}

/** Create endpoint from sockaddr_storage.

    Dispatches on `ss_family` to reconstruct the appropriate
    IPv4 or IPv6 endpoint.

    @param storage The sockaddr_storage with fields in network byte order.
    @return An endpoint with address and port extracted from storage.
*/
inline endpoint
from_sockaddr(sockaddr_storage const& storage) noexcept
{
    if (storage.ss_family == AF_INET)
    {
        sockaddr_in sa;
        std::memcpy(&sa, &storage, sizeof(sa));
        return from_sockaddr_in(sa);
    }
    if (storage.ss_family == AF_INET6)
    {
        sockaddr_in6 sa6;
        std::memcpy(&sa6, &storage, sizeof(sa6));
        return from_sockaddr_in6(sa6);
    }
    return endpoint{};
}

/** Return the native address family for an endpoint.

    @param ep The endpoint to query.
    @return `AF_INET` for IPv4, `AF_INET6` for IPv6.
*/
inline int
endpoint_family(endpoint const& ep) noexcept
{
    return ep.is_v6() ? AF_INET6 : AF_INET;
}

/** Return the address family of a socket descriptor.

    @param fd The socket file descriptor.
    @return AF_INET, AF_INET6, or AF_UNSPEC on failure.
*/
inline int
socket_family(
#if BOOST_COROSIO_POSIX
    int fd
#else
    std::uintptr_t fd
#endif
    ) noexcept
{
    sockaddr_storage storage{};
    socklen_t len = sizeof(storage);
    if (getsockname(
#if BOOST_COROSIO_POSIX
            fd,
#else
            static_cast<SOCKET>(fd),
#endif
            reinterpret_cast<sockaddr*>(&storage), &len) != 0)
        return AF_UNSPEC;
    return storage.ss_family;
}

//----------------------------------------------------------
// local_endpoint (AF_UNIX) conversions
//----------------------------------------------------------

#if BOOST_COROSIO_POSIX

/** Convert a local_endpoint to sockaddr_storage.

    @param ep The local endpoint to convert.
    @param storage Output parameter filled with the sockaddr_un.
    @return The length of the filled sockaddr structure.
*/
inline socklen_t
to_sockaddr(local_endpoint const& ep, sockaddr_storage& storage) noexcept
{
    std::memset(&storage, 0, sizeof(storage));
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    auto path     = ep.path();
    auto copy_len = (std::min)(path.size(), sizeof(sa.sun_path));
    if (copy_len > 0)
        std::memcpy(sa.sun_path, path.data(), copy_len);
    std::memcpy(&storage, &sa, sizeof(sa));

    if (ep.is_abstract())
        return static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + path.size());
    return static_cast<socklen_t>(sizeof(sa));
}

/** Convert a local_endpoint to sockaddr_storage (family-aware overload).

    The socket_family parameter is ignored for Unix sockets since
    there is no dual-stack mapping.

    @param ep The local endpoint to convert.
    @param socket_family Ignored.
    @param storage Output parameter filled with the sockaddr_un.
    @return The length of the filled sockaddr structure.
*/
inline socklen_t
to_sockaddr(
    local_endpoint const& ep,
    int /*socket_family*/,
    sockaddr_storage& storage) noexcept
{
    return to_sockaddr(ep, storage);
}

/** Create a local_endpoint from sockaddr_storage.

    @param storage The sockaddr_storage (must have ss_family == AF_UNIX).
    @param len The address length returned by the kernel.
    @return A local_endpoint with the path extracted from the
        sockaddr_un, or an empty endpoint if the family is not AF_UNIX.
*/
inline local_endpoint
from_sockaddr_local(
    sockaddr_storage const& storage, socklen_t len) noexcept
{
    if (storage.ss_family != AF_UNIX)
        return local_endpoint{};

    sockaddr_un sa{};
    auto bytes_copied =
        (std::min)(static_cast<std::size_t>(len), sizeof(sa));
    std::memcpy(&sa, &storage, bytes_copied);

    // Derive path_len from bytes_copied (NOT len) so memchr and the
    // string_view below can never read past sa.sun_path. The kernel
    // can return an addrlen larger than sizeof(sockaddr_un) (e.g.,
    // sizeof(sockaddr_storage) from a misbehaving caller); without
    // clamping, the OOB read could expose adjacent stack bytes.
    auto path_offset = offsetof(sockaddr_un, sun_path);
    if (bytes_copied <= path_offset)
        return local_endpoint{};

    auto path_len = bytes_copied - path_offset;

    // Non-abstract paths may be null-terminated by the kernel
    if (path_len > 0 && sa.sun_path[0] != '\0')
    {
        auto* end = static_cast<char const*>(
            std::memchr(sa.sun_path, '\0', path_len));
        if (end)
            path_len = static_cast<std::size_t>(end - sa.sun_path);
    }

    std::error_code ec;
    local_endpoint ep(std::string_view(sa.sun_path, path_len), ec);
    if (ec)
        return local_endpoint{};
    return ep;
}

#endif // BOOST_COROSIO_POSIX

//----------------------------------------------------------
// Tag-dispatch helpers for templatized reactor code.
// Overload resolution selects the correct conversion based
// on the Endpoint type.
//----------------------------------------------------------

/** Convert sockaddr_storage to an IP endpoint (tag overload).

    @param storage The sockaddr_storage with fields in network byte order.
    @param len The address length returned by the kernel.
    @return An endpoint with address and port extracted from storage.
*/
inline endpoint
from_sockaddr_as(
    sockaddr_storage const& storage, socklen_t /*len*/, endpoint const&) noexcept
{
    return from_sockaddr(storage);
}

#if BOOST_COROSIO_POSIX

/** Convert sockaddr_storage to a local_endpoint (tag overload).

    @param storage The sockaddr_storage.
    @param len The address length returned by the kernel.
    @return A local_endpoint with path extracted from storage.
*/
inline local_endpoint
from_sockaddr_as(
    sockaddr_storage const& storage,
    socklen_t len,
    local_endpoint const&) noexcept
{
    return from_sockaddr_local(storage, len);
}

#endif // BOOST_COROSIO_POSIX

} // namespace boost::corosio::detail

#endif
