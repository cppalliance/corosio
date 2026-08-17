//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_VALIDATE_FD_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_VALIDATE_FD_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/native/detail/make_err.hpp>

#include <cerrno>
#include <system_error>

#include <sys/socket.h>

namespace boost::corosio::detail {

/** Validate a caller-supplied socket fd for adoption.

    Non-mutating: interrogates the fd without changing any of its
    flags, so a rejected fd goes back to the caller untouched.

    @param fd The descriptor to validate.
    @param expected_type `SOCK_STREAM` or `SOCK_DGRAM`.
    @param is_ip Accept `AF_INET`/`AF_INET6` when true, `AF_UNIX`
        when false.
    @return Empty on success; `EBADF`, `EAFNOSUPPORT`, `EPROTOTYPE`,
        or the `errno` reported by the interrogating call.
*/
inline std::error_code
validate_socket_fd(int fd, int expected_type, bool is_ip) noexcept
{
    if (fd < 0)
        return make_err(EBADF);

    sockaddr_storage st{};
    socklen_t st_len = sizeof(st);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&st), &st_len) != 0)
        return make_err(errno);
    if (is_ip)
    {
        if (st.ss_family != AF_INET && st.ss_family != AF_INET6)
            return make_err(EAFNOSUPPORT);
    }
    else if (st.ss_family != AF_UNIX)
    {
        return make_err(EAFNOSUPPORT);
    }

    int sock_type = 0;
    socklen_t opt_len = sizeof(sock_type);
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE,
            &sock_type, &opt_len) != 0)
        return make_err(errno);
    if (sock_type != expected_type)
        return make_err(EPROTOTYPE);

    return {};
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_VALIDATE_FD_HPP
