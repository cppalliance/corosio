//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tcp.hpp>
#include <boost/corosio/native/native_tcp.hpp>

namespace boost::corosio {

int
tcp::family() const noexcept
{
    return native_tcp(v6_ ? native_tcp::v6() : native_tcp::v4()).family();
}

int
tcp::type() noexcept
{
    return native_tcp::type();
}

int
tcp::protocol() noexcept
{
    return native_tcp::protocol();
}

} // namespace boost::corosio
