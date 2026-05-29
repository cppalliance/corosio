//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/local_socket_pair.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio {

namespace {

#ifndef SOCK_NONBLOCK
void
make_nonblock_cloexec(int fd)
{
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        throw std::system_error(
            std::error_code(errno, std::system_category()),
            "fcntl(F_GETFL)");
    if (::fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        throw std::system_error(
            std::error_code(errno, std::system_category()),
            "fcntl(F_SETFL)");
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        throw std::system_error(
            std::error_code(errno, std::system_category()),
            "fcntl(F_SETFD)");
}
#endif

void
create_pair(int type, int fds[2])
{
    int flags = type;
#ifdef SOCK_NONBLOCK
    flags |= SOCK_NONBLOCK | SOCK_CLOEXEC;
#endif
    if (::socketpair(AF_UNIX, flags, 0, fds) != 0)
        throw std::system_error(
            std::error_code(errno, std::system_category()),
            "socketpair");
#ifndef SOCK_NONBLOCK
    try
    {
        make_nonblock_cloexec(fds[0]);
        make_nonblock_cloexec(fds[1]);
    }
    catch (...)
    {
        ::close(fds[0]);
        ::close(fds[1]);
        throw;
    }
#endif
}

} // namespace

std::pair<local_stream_socket, local_stream_socket>
make_local_stream_pair(io_context& ctx)
{
    int fds[2];
    create_pair(SOCK_STREAM, fds);

    try
    {
        local_stream_socket s1(ctx);
        local_stream_socket s2(ctx);

        s1.assign(fds[0]);
        fds[0] = -1;
        s2.assign(fds[1]);
        fds[1] = -1;

        return {std::move(s1), std::move(s2)};
    }
    catch (...)
    {
        if (fds[0] >= 0)
            ::close(fds[0]);
        if (fds[1] >= 0)
            ::close(fds[1]);
        throw;
    }
}

std::pair<local_datagram_socket, local_datagram_socket>
make_local_datagram_pair(io_context& ctx)
{
    int fds[2];
    create_pair(SOCK_DGRAM, fds);

    try
    {
        local_datagram_socket s1(ctx);
        local_datagram_socket s2(ctx);

        s1.assign(fds[0]);
        fds[0] = -1;
        s2.assign(fds[1]);
        fds[1] = -1;

        return {std::move(s1), std::move(s2)};
    }
    catch (...)
    {
        if (fds[0] >= 0)
            ::close(fds[0]);
        if (fds[1] >= 0)
            ::close(fds[1]);
        throw;
    }
}

} // namespace boost::corosio

#elif BOOST_COROSIO_HAS_IOCP

// Windows: emulate socketpair(AF_UNIX, SOCK_STREAM) via a temporary
// listener socket.  Create a listening socket bound to a unique temp
// path, connect a client, accept the peer, then close the listener
// and remove the temp path.

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/test/temp_path.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/native/detail/endpoint_convert.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>

#include <system_error>
#include <utility>

namespace boost::corosio {

namespace {

std::error_code
make_wsa_error()
{
    return std::error_code(::WSAGetLastError(), std::system_category());
}

} // namespace

std::pair<local_stream_socket, local_stream_socket>
make_local_stream_pair(io_context& ctx)
{
    // Create a unique temp directory + path for the listener.
    test::temp_socket_dir tmp;
    auto path = tmp.path();
    local_endpoint ep(path);

    // Build the sockaddr.
    sockaddr_storage storage{};
    socklen_t addrlen = detail::to_sockaddr(ep, storage);

    // Create listener socket.
    SOCKET listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET)
        throw std::system_error(make_wsa_error(), "socket(listener)");

    // bind + listen
    if (::bind(listener, reinterpret_cast<sockaddr*>(&storage),
               static_cast<int>(addrlen)) == SOCKET_ERROR)
    {
        auto ec = make_wsa_error();
        ::closesocket(listener);
        throw std::system_error(ec, "bind(listener)");
    }

    if (::listen(listener, 1) == SOCKET_ERROR)
    {
        auto ec = make_wsa_error();
        ::closesocket(listener);
        throw std::system_error(ec, "listen");
    }

    // Create client socket and connect.
    SOCKET client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (client == INVALID_SOCKET)
    {
        auto ec = make_wsa_error();
        ::closesocket(listener);
        throw std::system_error(ec, "socket(client)");
    }

    if (::connect(client, reinterpret_cast<sockaddr*>(&storage),
                  static_cast<int>(addrlen)) == SOCKET_ERROR)
    {
        auto ec = make_wsa_error();
        ::closesocket(client);
        ::closesocket(listener);
        throw std::system_error(ec, "connect");
    }

    // Accept the peer.
    SOCKET server = ::accept(listener, nullptr, nullptr);
    if (server == INVALID_SOCKET)
    {
        auto ec = make_wsa_error();
        ::closesocket(client);
        ::closesocket(listener);
        throw std::system_error(ec, "accept");
    }

    // Listener is no longer needed.
    ::closesocket(listener);

    // Wrap the raw SOCKETs into local_stream_socket objects.
    // assign() registers the handle with the IOCP port.
    try
    {
        local_stream_socket s1(ctx);
        local_stream_socket s2(ctx);

        s1.assign(static_cast<native_handle_type>(client));
        client = INVALID_SOCKET;
        s2.assign(static_cast<native_handle_type>(server));
        server = INVALID_SOCKET;

        return {std::move(s1), std::move(s2)};
    }
    catch (...)
    {
        if (client != INVALID_SOCKET)
            ::closesocket(client);
        if (server != INVALID_SOCKET)
            ::closesocket(server);
        throw;
    }
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP
