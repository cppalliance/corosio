//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/error.hpp>
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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <new>
#include <random>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

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
    if (auto ec = a.assign(a_fd))
    {
        ::close(a_fd);
        ::close(b_fd);
        return ec;
    }

    if (auto ec = b.assign(b_fd))
    {
        a.close();
        ::close(b_fd);
        return ec;
    }

    return {};
}

#elif BOOST_COROSIO_HAS_IOCP

// random_device may throw, and a predictable seed only costs a retry
// on a name collision.
std::uint64_t
pair_seed() noexcept
{
    try
    {
        return std::random_device{}();
    }
    catch (...)
    {
        LARGE_INTEGER t{};
        ::QueryPerformanceCounter(&t);
        return static_cast<std::uint64_t>(t.QuadPart) ^
            (static_cast<std::uint64_t>(::GetCurrentProcessId()) << 32) ^
            ::GetCurrentThreadId();
    }
}

// Build a unique sub-directory under temp and the socket path inside
// it.
std::error_code
pick_pair_path(
    std::filesystem::path& dir_out,
    std::filesystem::path& file_out,
    std::string& path_out) noexcept
{
    namespace fs = std::filesystem;

    thread_local std::mt19937_64 gen{pair_seed()};

    try
    {
        std::error_code ec;
        fs::path const tmp = fs::temp_directory_path(ec);
        if (ec)
            return ec;

        for (int attempt = 0; attempt < 16; ++attempt)
        {
            fs::path dir  = tmp / ("co_pair_" + std::to_string(gen()));
            fs::path file = dir / "s";
            // Converted before the directory exists, so a throw here
            // leaves nothing on disk to remove.
            std::string path = file.string();
            if (fs::create_directory(dir, ec))
            {
                dir_out  = std::move(dir);
                file_out = std::move(file);
                path_out = std::move(path);
                return {};
            }
        }
        if (ec)
            return ec;
        return detail::make_err(ERROR_PATH_NOT_FOUND);
    }
    catch (std::system_error const& e)
    {
        return e.code();
    }
    catch (std::bad_alloc const&)
    {
        return std::make_error_code(std::errc::not_enough_memory);
    }
}

void
remove_pair_path(
    std::filesystem::path const& dir,
    std::filesystem::path const& file) noexcept
{
    std::error_code ec;
    std::filesystem::remove(file, ec);
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

    fs::path dir;
    fs::path file;
    std::string path;
    if (auto ec = pick_pair_path(dir, file, path))
        return ec;

    SOCKET listen_sock =
        ::WSASocketW(AF_UNIX, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (listen_sock == INVALID_SOCKET)
    {
        auto ec = detail::make_err(::WSAGetLastError());
        remove_pair_path(dir, file);
        return ec;
    }

    detail::un_sa_t addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(
        addr.sun_path, path.c_str(),
        (std::min)(path.size(), sizeof(addr.sun_path) - 1));
    int addr_len =
        static_cast<int>(offsetof(detail::un_sa_t, sun_path) + path.size() + 1);

    if (::bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), addr_len) ==
        SOCKET_ERROR)
    {
        auto ec = detail::make_err(::WSAGetLastError());
        ::closesocket(listen_sock);
        remove_pair_path(dir, file);
        return ec;
    }

    if (::listen(listen_sock, 1) == SOCKET_ERROR)
    {
        auto ec = detail::make_err(::WSAGetLastError());
        ::closesocket(listen_sock);
        remove_pair_path(dir, file);
        return ec;
    }

    // A worker that fails before connecting produces no connection at
    // all, so the accept below must be able to give up. Poll the
    // listener instead of blocking in accept() forever.
    u_long non_blocking = 1;
    if (::ioctlsocket(listen_sock, FIONBIO, &non_blocking) == SOCKET_ERROR)
    {
        auto ec = detail::make_err(::WSAGetLastError());
        ::closesocket(listen_sock);
        remove_pair_path(dir, file);
        return ec;
    }

    SOCKET worker_sock = INVALID_SOCKET;
    std::error_code worker_ec;
    std::atomic<bool> worker_done{false};

    // One exit, so worker_done is published on every path: the accept
    // below waits on it, and a path that skipped it would hang.
    auto connect_worker = [&] {
        worker_sock = ::WSASocketW(
            AF_UNIX, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (worker_sock == INVALID_SOCKET)
        {
            worker_ec = detail::make_err(::WSAGetLastError());
        }
        else
        {
            detail::un_sa_t caddr{};
            caddr.sun_family = AF_UNIX;
            std::memcpy(
                caddr.sun_path, path.c_str(),
                (std::min)(path.size(), sizeof(caddr.sun_path) - 1));
            int caddr_len = static_cast<int>(
                offsetof(detail::un_sa_t, sun_path) + path.size() + 1);

            if (::connect(
                    worker_sock, reinterpret_cast<sockaddr*>(&caddr),
                    caddr_len) == SOCKET_ERROR)
            {
                worker_ec = detail::make_err(::WSAGetLastError());
                ::closesocket(worker_sock);
                worker_sock = INVALID_SOCKET;
            }
        }
        // Released last so a reader that sees it also sees worker_ec.
        worker_done.store(true, std::memory_order_release);
    };

    // A thread that never starts publishes nothing, so this returns
    // before the accept loop could wait on it.
    std::thread worker;
    std::error_code thread_ec;
    try
    {
        worker = std::thread(connect_worker);
    }
    catch (std::system_error const& e)
    {
        thread_ec = e.code();
    }
    catch (std::bad_alloc const&)
    {
        thread_ec = std::make_error_code(std::errc::not_enough_memory);
    }
    catch (...)
    {
    }
    if (!worker.joinable())
    {
        if (!thread_ec)
        {
            thread_ec =
                std::make_error_code(std::errc::resource_unavailable_try_again);
        }
        ::closesocket(listen_sock);
        remove_pair_path(dir, file);
        return thread_ec;
    }

    SOCKET accept_sock = INVALID_SOCKET;
    std::error_code accept_ec;
    for (;;)
    {
        // A worker that succeeded has left a connection in the
        // backlog, so only a failed one means nothing is coming.
        if (worker_done.load(std::memory_order_acquire) && worker_ec)
            break;

        WSAPOLLFD pfd{listen_sock, POLLRDNORM, 0};
        int const n = ::WSAPoll(&pfd, 1, 100);
        if (n == SOCKET_ERROR)
        {
            accept_ec = detail::make_err(::WSAGetLastError());
            break;
        }
        if (n == 0)
            continue;

        // Readiness that is not "a connection is waiting" is an error
        // condition on the listener; accepting on it would spin. The
        // condition carries no retrievable code, so this one is
        // corosio's own and has to compare equal on every toolchain.
        if ((pfd.revents & POLLRDNORM) == 0)
        {
            accept_ec = std::make_error_code(std::errc::connection_aborted);
            break;
        }

        accept_sock = ::accept(listen_sock, nullptr, nullptr);
        if (accept_sock != INVALID_SOCKET)
            break;
        DWORD const err = ::WSAGetLastError();
        // A readiness report with nothing left to accept: keep
        // waiting for the worker's connection.
        if (err == WSAEWOULDBLOCK)
            continue;
        accept_ec = detail::make_err(err);
        break;
    }

    worker.join();

    ::closesocket(listen_sock);
    remove_pair_path(dir, file);

    if (accept_ec)
    {
        if (worker_sock != INVALID_SOCKET)
            ::closesocket(worker_sock);
        return accept_ec;
    }
    if (worker_ec)
    {
        if (accept_sock != INVALID_SOCKET)
            ::closesocket(accept_sock);
        return worker_ec;
    }

    // accept() inherits the listener's non-blocking mode; the rest of
    // the IOCP backend hands out blocking sockets and drives them
    // through overlapped I/O.
    non_blocking = 0;
    if (::ioctlsocket(accept_sock, FIONBIO, &non_blocking) == SOCKET_ERROR)
    {
        auto ec = detail::make_err(::WSAGetLastError());
        ::closesocket(accept_sock);
        ::closesocket(worker_sock);
        return ec;
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
    if (auto ec = a.assign(static_cast<native_handle_type>(a_sock)))
    {
        ::closesocket(a_sock);
        ::closesocket(b_sock);
        return ec;
    }

    if (auto ec = b.assign(static_cast<native_handle_type>(b_sock)))
    {
        a.close();
        ::closesocket(b_sock);
        return ec;
    }

    return {};
}

#endif

} // namespace

std::error_code
connect_pair(local_stream_socket& a, local_stream_socket& b) noexcept
{
    if (a.is_open() || b.is_open())
        return make_error_code(error::already_open);

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
        return make_error_code(error::already_open);

    int a_fd = -1, b_fd = -1;
    if (auto ec = make_pair_fds(SOCK_DGRAM, a_fd, b_fd))
        return ec;
    return assign_pair(a, b, a_fd, b_fd);
}

#endif

} // namespace boost::corosio
