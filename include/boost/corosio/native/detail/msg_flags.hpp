//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_MSG_FLAGS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_MSG_FLAGS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX
#include <sys/socket.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#endif

namespace boost::corosio::detail {

/// Map portable message_flags int values to native MSG_* constants.
inline int
to_native_msg_flags(int flags) noexcept
{
    int native = 0;
    if (flags & 1) native |= MSG_PEEK;
    if (flags & 2) native |= MSG_OOB;
    if (flags & 4) native |= MSG_DONTROUTE;
    return native;
}

} // namespace boost::corosio::detail

#endif
