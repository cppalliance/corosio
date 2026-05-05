//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_TRAITS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_TRAITS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>

#include <system_error>

#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* epoll backend traits.

   Captures the platform-specific behavior of the Linux epoll backend:
   atomic SOCK_NONBLOCK|SOCK_CLOEXEC on socket(), accept4() for
   accepted connections, and sendmsg(MSG_NOSIGNAL) for writes.
*/

namespace boost::corosio::detail {

class epoll_scheduler;

struct epoll_traits
{
    using scheduler_type    = epoll_scheduler;
    using desc_state_type   = reactor_descriptor_state;

    static constexpr bool needs_write_notification = false;

    // No extra per-socket state or lifecycle hooks needed for epoll.
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

            ssize_t n;
            do
            {
                n = ::sendmsg(fd, &msg, MSG_NOSIGNAL);
            }
            while (n < 0 && errno == EINTR);
            return n;
        }

        static ssize_t write_one(
            int fd, void const* data, std::size_t size) noexcept
        {
            ssize_t n;
            do
            {
                n = ::send(fd, data, size, MSG_NOSIGNAL);
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
                new_fd = ::accept4(
                    fd, reinterpret_cast<sockaddr*>(&peer), &addrlen,
                    SOCK_NONBLOCK | SOCK_CLOEXEC);
            }
            while (new_fd < 0 && errno == EINTR);
            return new_fd;
        }
    };

    // Create a nonblocking, close-on-exec socket using Linux's atomic flags.
    static int create_socket(int family, int type, int protocol) noexcept
    {
        return ::socket(family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
    }

    // Apply protocol-specific options after socket creation.
    // For IP sockets, sets IPV6_V6ONLY on AF_INET6 (best-effort).
    static std::error_code
    configure_ip_socket(int fd, int family) noexcept
    {
        if (family == AF_INET6)
        {
            int one = 1;
            (void)::setsockopt(
                fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
        }
        return {};
    }

    // Apply protocol-specific options for acceptor sockets.
    // For IP acceptors, sets IPV6_V6ONLY=0 (dual-stack, best-effort).
    static std::error_code
    configure_ip_acceptor(int fd, int family) noexcept
    {
        if (family == AF_INET6)
        {
            int val = 0;
            (void)::setsockopt(
                fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof(val));
        }
        return {};
    }

    // No extra configuration needed for local (unix) sockets on epoll.
    static std::error_code
    configure_local_socket(int /*fd*/) noexcept
    {
        return {};
    }

    // Non-mutating validation for fds adopted via assign(). Used when
    // the caller retains fd ownership responsibility.
    static std::error_code
    validate_assigned_fd(int /*fd*/) noexcept
    {
        return {};
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_TRAITS_HPP
