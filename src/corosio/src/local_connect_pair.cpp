//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/native/detail/make_err.hpp>

#include <system_error>

#if BOOST_COROSIO_POSIX
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#elif BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/endpoint_convert.hpp>

#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>

#ifndef AF_UNIX
#define AF_UNIX 1
#endif
#endif

namespace boost::corosio {

namespace {

#if BOOST_COROSIO_POSIX

std::error_code
make_pair_fds(int type, int& a_fd, int& b_fd) noexcept
{
    int fds[2];
    if (::socketpair(AF_UNIX, type, 0, fds) != 0)
        return detail::make_err(errno);

    // assign() is documented "adopt-only" and will not mutate the fd;
    // set O_NONBLOCK before transferring ownership.
    for (int i = 0; i < 2; ++i)
    {
        int flags = ::fcntl(fds[i], F_GETFL, 0);
        if (flags < 0 || ::fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) < 0)
        {
            auto ec = detail::make_err(errno);
            ::close(fds[0]);
            ::close(fds[1]);
            return ec;
        }
    }

    a_fd = fds[0];
    b_fd = fds[1];
    return {};
}

template<class Socket>
std::error_code
assign_pair(Socket& a, Socket& b, int a_fd, int b_fd) noexcept
{
    try
    {
        a.assign(a_fd);
    }
    catch (std::system_error const& e)
    {
        ::close(a_fd);
        ::close(b_fd);
        return e.code();
    }

    try
    {
        b.assign(b_fd);
    }
    catch (std::system_error const& e)
    {
        a.close();
        ::close(b_fd);
        return e.code();
    }

    return {};
}

#elif BOOST_COROSIO_HAS_IOCP

// Build a unique sub-directory under temp and return the full socket
// path inside it. Empty string on failure.
std::string
pick_pair_path(std::filesystem::path& dir_out)
{
    namespace fs = std::filesystem;

    thread_local std::mt19937_64 gen{std::random_device{}()};

    for (int attempt = 0; attempt < 16; ++attempt)
    {
        auto candidate =
            fs::temp_directory_path() /
            ("co_pair_" + std::to_string(gen()));
        std::error_code ec;
        if (fs::create_directory(candidate, ec))
        {
            dir_out = candidate;
            return (candidate / "s").string();
        }
    }
    return {};
}

void
remove_pair_path(std::filesystem::path const& dir, std::string const& path)
{
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(path), ec);
    std::filesystem::remove(dir, ec);
}

// Synchronously rendezvous two AF_UNIX SOCK_STREAM sockets. The
// listener and accept happen on the caller's thread; the connect
// runs on a short-lived worker to avoid a deadlock. The returned
// sockets are created with WSA_FLAG_OVERLAPPED so they can be
// registered with IOCP by assign_socket().
std::error_code
make_pair_sockets(SOCKET& a_sock, SOCKET& b_sock) noexcept
{
    namespace fs = std::filesystem;

    a_sock = INVALID_SOCKET;
    b_sock = INVALID_SOCKET;

    fs::path    dir;
    std::string path = pick_pair_path(dir);
    if (path.empty())
        return detail::make_err(ERROR_PATH_NOT_FOUND);

    SOCKET listen_sock = ::WSASocketW(
        AF_UNIX, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (listen_sock == INVALID_SOCKET)
    {
        auto ec = detail::make_err(::WSAGetLastError());
        remove_pair_path(dir, path);
        return ec;
    }

    detail::un_sa_t addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(
        addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    int addr_len = static_cast<int>(
        offsetof(detail::un_sa_t, sun_path) + path.size() + 1);

    if (::bind(
            listen_sock, reinterpret_cast<sockaddr*>(&addr), addr_len)
        == SOCKET_ERROR)
    {
        auto ec = detail::make_err(::WSAGetLastError());
        ::closesocket(listen_sock);
        remove_pair_path(dir, path);
        return ec;
    }

    if (::listen(listen_sock, 1) == SOCKET_ERROR)
    {
        auto ec = detail::make_err(::WSAGetLastError());
        ::closesocket(listen_sock);
        remove_pair_path(dir, path);
        return ec;
    }

    SOCKET          worker_sock = INVALID_SOCKET;
    std::error_code worker_ec;

    std::thread worker([&] {
        worker_sock = ::WSASocketW(
            AF_UNIX, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (worker_sock == INVALID_SOCKET)
        {
            worker_ec = detail::make_err(::WSAGetLastError());
            return;
        }

        detail::un_sa_t caddr{};
        caddr.sun_family = AF_UNIX;
        std::strncpy(
            caddr.sun_path, path.c_str(), sizeof(caddr.sun_path) - 1);
        int caddr_len = static_cast<int>(
            offsetof(detail::un_sa_t, sun_path) + path.size() + 1);

        if (::connect(
                worker_sock, reinterpret_cast<sockaddr*>(&caddr), caddr_len)
            == SOCKET_ERROR)
        {
            worker_ec = detail::make_err(::WSAGetLastError());
            ::closesocket(worker_sock);
            worker_sock = INVALID_SOCKET;
        }
    });

    SOCKET          accept_sock = ::accept(listen_sock, nullptr, nullptr);
    std::error_code accept_ec;
    if (accept_sock == INVALID_SOCKET)
        accept_ec = detail::make_err(::WSAGetLastError());

    worker.join();

    ::closesocket(listen_sock);
    remove_pair_path(dir, path);

    if (accept_ec)
    {
        if (worker_sock != INVALID_SOCKET)
            ::closesocket(worker_sock);
        return accept_ec;
    }
    if (worker_ec)
    {
        ::closesocket(accept_sock);
        return worker_ec;
    }

    a_sock = accept_sock;
    b_sock = worker_sock;
    return {};
}

std::error_code
assign_pair(
    local_stream_socket& a,
    local_stream_socket& b,
    SOCKET a_sock,
    SOCKET b_sock) noexcept
{
    try
    {
        a.assign(static_cast<native_handle_type>(a_sock));
    }
    catch (std::system_error const& e)
    {
        ::closesocket(a_sock);
        ::closesocket(b_sock);
        return e.code();
    }

    try
    {
        b.assign(static_cast<native_handle_type>(b_sock));
    }
    catch (std::system_error const& e)
    {
        a.close();
        ::closesocket(b_sock);
        return e.code();
    }

    return {};
}

#endif

} // namespace

std::error_code
connect_pair(local_stream_socket& a, local_stream_socket& b) noexcept
{
    if (a.is_open() || b.is_open())
        return detail::make_err(
#if BOOST_COROSIO_POSIX
            EISCONN
#else
            WSAEISCONN
#endif
        );

#if BOOST_COROSIO_POSIX
    int a_fd = -1, b_fd = -1;
    if (auto ec = make_pair_fds(SOCK_STREAM, a_fd, b_fd))
        return ec;
    return assign_pair(a, b, a_fd, b_fd);
#elif BOOST_COROSIO_HAS_IOCP
    SOCKET a_sock = INVALID_SOCKET, b_sock = INVALID_SOCKET;
    if (auto ec = make_pair_sockets(a_sock, b_sock))
        return ec;
    return assign_pair(a, b, a_sock, b_sock);
#else
    return detail::make_err(ENOSYS);
#endif
}

#if BOOST_COROSIO_POSIX

std::error_code
connect_pair(local_datagram_socket& a, local_datagram_socket& b) noexcept
{
    if (a.is_open() || b.is_open())
        return detail::make_err(EISCONN);

    int a_fd = -1, b_fd = -1;
    if (auto ec = make_pair_fds(SOCK_DGRAM, a_fd, b_fd))
        return ec;
    return assign_pair(a, b, a_fd, b_fd);
}

#endif

} // namespace boost::corosio
