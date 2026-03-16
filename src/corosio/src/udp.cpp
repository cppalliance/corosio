//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/udp.hpp>
#include <boost/corosio/native/native_udp.hpp>

namespace boost::corosio {

int
udp::family() const noexcept
{
    return native_udp(v6_ ? native_udp::v6() : native_udp::v4()).family();
}

int
udp::type() noexcept
{
    return native_udp::type();
}

int
udp::protocol() noexcept
{
    return native_udp::protocol();
}

} // namespace boost::corosio
