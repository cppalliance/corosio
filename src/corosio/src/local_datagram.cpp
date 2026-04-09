//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/local_datagram.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX
#include <sys/socket.h>
#include <sys/un.h>
#endif

namespace boost::corosio {

int
local_datagram::family() noexcept
{
#if BOOST_COROSIO_POSIX
    return AF_UNIX;
#else
    return 0;
#endif
}

int
local_datagram::type() noexcept
{
#if BOOST_COROSIO_POSIX
    return SOCK_DGRAM;
#else
    return 0;
#endif
}

int
local_datagram::protocol() noexcept
{
    return 0;
}

} // namespace boost::corosio
