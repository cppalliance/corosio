//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_SRC_DETAIL_OPEN_LISTENER_HPP
#define BOOST_COROSIO_SRC_DETAIL_OPEN_LISTENER_HPP

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_acceptor.hpp>

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_windows.hpp>
#endif

#include <cstddef>
#include <system_error>

namespace boost::corosio::detail {

#if BOOST_COROSIO_HAS_IOCP
// On Windows SO_REUSEADDR grants bind-over ("hijack") rights instead
// of TIME_WAIT reuse, so a second listener would share the port
// silently. SO_EXCLUSIVEADDRUSE restores the POSIX contract: the
// collision surfaces as WSAEADDRINUSE (errc::address_in_use).
struct exclusive_address_use
{
    int value_ = 1;

    static int level(family) noexcept
    {
        return SOL_SOCKET;
    }
    static int name(family) noexcept
    {
        return SO_EXCLUSIVEADDRUSE;
    }
    void const* data(family) const noexcept
    {
        return &value_;
    }
    std::size_t size(family) const noexcept
    {
        return sizeof(value_);
    }
};
#endif

// Open, configure address reuse, bind and listen, stopping at the
// first failing step.
inline std::error_code
open_listener(tcp_acceptor& acc, endpoint ep, int backlog) noexcept
{
    if (auto ec = acc.open(ep.address().family()))
        return ec;
#if BOOST_COROSIO_HAS_IOCP
    if (auto ec = acc.set_option(exclusive_address_use{}))
        return ec;
#else
    if (auto ec = acc.set_option(socket_option::reuse_address(true)))
        return ec;
#endif
    if (auto ec = acc.bind(ep))
        return ec;
    return acc.listen(backlog);
}

} // namespace boost::corosio::detail

#endif
