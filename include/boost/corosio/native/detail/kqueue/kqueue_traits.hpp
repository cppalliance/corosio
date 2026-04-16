//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_TRAITS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_TRAITS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>

#include <system_error>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

/* kqueue backend traits.

   Captures the platform-specific behavior of the BSD/macOS kqueue backend:
   manual fcntl for O_NONBLOCK/FD_CLOEXEC, mandatory SO_NOSIGPIPE (macOS
   lacks MSG_NOSIGNAL), writev() for writes, and accept()+fcntl for
   accepted connections.
*/

namespace boost::corosio::detail {

class kqueue_scheduler;

struct kqueue_traits
{
    using scheduler_type    = kqueue_scheduler;
    using desc_state_type   = reactor_descriptor_state;

    static constexpr bool needs_write_notification = false;

    /* macOS kqueue workaround: RST doesn't reliably trigger EV_EOF.
       If the user sets SO_LINGER, we clear it before close so the
       destructor doesn't block and close() sends FIN instead of RST.

       The hook tracks whether the user explicitly set SO_LINGER via
       set_option(). On pre_shutdown/pre_destroy, if the flag is set,
       we reset linger to off before the fd is closed.
    */
    struct stream_socket_hook
    {
        bool user_set_linger_ = false;

        std::error_code on_set_option(
            int fd, int level, int optname,
            void const* data, std::size_t size) noexcept
        {
            if (::setsockopt(
                    fd, level, optname, data,
                    static_cast<socklen_t>(size)) != 0)
                return make_err(errno);

            if (level == SOL_SOCKET && optname == SO_LINGER &&
                size >= sizeof(struct ::linger))
                user_set_linger_ =
                    static_cast<struct ::linger const*>(data)->l_onoff != 0;

            return {};
        }

        void pre_shutdown(int fd) noexcept
        {
            reset_linger(fd);
        }

        void pre_destroy(int fd) noexcept
        {
            reset_linger(fd);
        }

    private:
        void reset_linger(int fd) noexcept
        {
            if (user_set_linger_ && fd >= 0)
            {
                struct ::linger lg;
                lg.l_onoff  = 0;
                lg.l_linger = 0;
                ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
            }
            user_set_linger_ = false;
        }
    };

    struct write_policy
    {
        static ssize_t write(int fd, iovec* iovecs, int count) noexcept
        {
            ssize_t n;
            do
            {
                n = ::writev(fd, iovecs, count);
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
            int new_fd;
            do
            {
                addrlen = sizeof(peer);
                new_fd = ::accept(
                    fd, reinterpret_cast<sockaddr*>(&peer), &addrlen);
            }
            while (new_fd < 0 && errno == EINTR);

            if (new_fd < 0)
                return new_fd;

            int flags = ::fcntl(new_fd, F_GETFL, 0);
            if (flags == -1 ||
                ::fcntl(new_fd, F_SETFL, flags | O_NONBLOCK) == -1)
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

#ifndef BOOST_COROSIO_MRDOCS
            // SO_NOSIGPIPE is mandatory on kqueue platforms (macOS lacks
            // MSG_NOSIGNAL). Skipped under MRDOCS so the docs build can
            // parse this header on Linux, where SO_NOSIGPIPE is absent.
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

    // Create a plain socket. Fd options are applied by configure_*().
    static int create_socket(int family, int type, int protocol) noexcept
    {
        return ::socket(family, type, protocol);
    }

    // Set O_NONBLOCK, FD_CLOEXEC, and SO_NOSIGPIPE on a new fd.
    // Caller is responsible for closing fd on error.
    static std::error_code set_fd_options(int fd) noexcept
    {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags == -1)
            return make_err(errno);
        if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
            return make_err(errno);
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
            return make_err(errno);

#ifndef BOOST_COROSIO_MRDOCS
        // SO_NOSIGPIPE is mandatory on kqueue platforms (see accept_policy).
        int one = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0)
            return make_err(errno);
#endif

        return {};
    }

    // Apply protocol-specific options after socket creation.
    // For IP sockets, sets IPV6_V6ONLY on AF_INET6 (best-effort).
    static std::error_code
    configure_ip_socket(int fd, int family) noexcept
    {
        auto ec = set_fd_options(fd);
        if (ec)
            return ec;

        if (family == AF_INET6)
        {
            int v6only = 1;
            (void)::setsockopt(
                fd, IPPROTO_IPV6, IPV6_V6ONLY,
                &v6only, sizeof(v6only));
        }
        return {};
    }

    // Apply protocol-specific options for acceptor sockets.
    // For IP acceptors, sets IPV6_V6ONLY=0 (dual-stack, best-effort).
    static std::error_code
    configure_ip_acceptor(int fd, int family) noexcept
    {
        auto ec = set_fd_options(fd);
        if (ec)
            return ec;

        if (family == AF_INET6)
        {
            int val = 0;
            (void)::setsockopt(
                fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof(val));
        }
        return {};
    }

    // Apply options for local (unix) sockets.
    static std::error_code
    configure_local_socket(int fd) noexcept
    {
        return set_fd_options(fd);
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

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_TRAITS_HPP
