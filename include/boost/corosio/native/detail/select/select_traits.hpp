//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_TRAITS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_TRAITS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>

#include <system_error>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

/* select backend traits.

   Captures the platform-specific behavior of the portable select() backend:
   manual fcntl for O_NONBLOCK/FD_CLOEXEC, FD_SETSIZE validation,
   conditional SO_NOSIGPIPE, sendmsg(MSG_NOSIGNAL) where available,
   and accept()+fcntl for accepted connections.
*/

namespace boost::corosio::detail {

class select_scheduler;
struct select_descriptor_state;

struct select_traits
{
    using scheduler_type    = select_scheduler;
    using desc_state_type   = select_descriptor_state;

    static constexpr bool needs_write_notification = true;

    /// No extra per-socket state or lifecycle hooks needed for select.
    struct stream_socket_hook
    {
        std::error_code on_set_option(
            int fd, int level, int optname,
            void const* data, std::size_t size) noexcept
        {
            if (::setsockopt(
                    fd, level, optname, data,
                    static_cast<socklen_t>(size)) != 0)
                return make_err(errno);
            return {};
        }
        static void pre_shutdown(int) noexcept {}
        static void pre_destroy(int) noexcept {}
    };

    struct write_policy
    {
        static ssize_t write(int fd, iovec* iovecs, int count) noexcept
        {
            msghdr msg{};
            msg.msg_iov    = iovecs;
            msg.msg_iovlen = static_cast<std::size_t>(count);

#ifdef MSG_NOSIGNAL
            constexpr int send_flags = MSG_NOSIGNAL;
#else
            constexpr int send_flags = 0;
#endif

            ssize_t n;
            do
            {
                n = ::sendmsg(fd, &msg, send_flags);
            }
            while (n < 0 && errno == EINTR);
            return n;
        }
    };

    struct accept_policy
    {
        static int do_accept(
            int fd, sockaddr_storage& peer, socklen_t& addrlen) noexcept
        {
            addrlen = sizeof(peer);
            int new_fd;
            do
            {
                new_fd = ::accept(
                    fd, reinterpret_cast<sockaddr*>(&peer), &addrlen);
            }
            while (new_fd < 0 && errno == EINTR);

            if (new_fd < 0)
                return new_fd;

            if (new_fd >= FD_SETSIZE)
            {
                ::close(new_fd);
                errno = EINVAL;
                return -1;
            }

            int flags = ::fcntl(new_fd, F_GETFL, 0);
            if (flags == -1)
            {
                int err = errno;
                ::close(new_fd);
                errno = err;
                return -1;
            }

            if (::fcntl(new_fd, F_SETFL, flags | O_NONBLOCK) == -1)
            {
                int err = errno;
                ::close(new_fd);
                errno = err;
                return -1;
            }

            if (::fcntl(new_fd, F_SETFD, FD_CLOEXEC) == -1)
            {
                int err = errno;
                ::close(new_fd);
                errno = err;
                return -1;
            }

#ifdef SO_NOSIGPIPE
            int one = 1;
            if (::setsockopt(
                    new_fd, SOL_SOCKET, SO_NOSIGPIPE,
                    &one, sizeof(one)) == -1)
            {
                int err = errno;
                ::close(new_fd);
                errno = err;
                return -1;
            }
#endif

            return new_fd;
        }
    };

    /// Create a plain socket (no atomic flags — select is POSIX-portable).
    static int create_socket(int family, int type, int protocol) noexcept
    {
        return ::socket(family, type, protocol);
    }

    /// Set O_NONBLOCK, FD_CLOEXEC; check FD_SETSIZE; optionally SO_NOSIGPIPE.
    /// Caller is responsible for closing fd on error.
    static std::error_code set_fd_options(int fd) noexcept
    {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags == -1)
            return make_err(errno);
        if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
            return make_err(errno);
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
            return make_err(errno);

        if (fd >= FD_SETSIZE)
            return make_err(EINVAL);

#ifdef SO_NOSIGPIPE
        // SO_NOSIGPIPE is the primary defense against SIGPIPE on
        // platforms that don't support MSG_NOSIGNAL. A silent failure
        // here would leave the fd vulnerable to crashing the process
        // on a closed-peer write, so propagate the error.
        {
            int one = 1;
            if (::setsockopt(
                    fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0)
                return make_err(errno);
        }
#endif

        return {};
    }

    /// Apply protocol-specific options after socket creation.
    /// For IP sockets, sets IPV6_V6ONLY on AF_INET6.
    static std::error_code
    configure_ip_socket(int fd, int family) noexcept
    {
        if (family == AF_INET6)
        {
            int one = 1;
            if (::setsockopt(
                    fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) != 0)
                return make_err(errno);
        }

        return set_fd_options(fd);
    }

    /// Apply protocol-specific options for acceptor sockets.
    /// For IP acceptors, sets IPV6_V6ONLY=0 (dual-stack).
    static std::error_code
    configure_ip_acceptor(int fd, int family) noexcept
    {
        if (family == AF_INET6)
        {
            int val = 0;
            if (::setsockopt(
                    fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof(val)) != 0)
                return make_err(errno);
        }

        return set_fd_options(fd);
    }

    /// Apply options for local (unix) sockets.
    static std::error_code
    configure_local_socket(int fd) noexcept
    {
        return set_fd_options(fd);
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_TRAITS_HPP
