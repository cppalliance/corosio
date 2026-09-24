//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_FAMILY_HPP
#define BOOST_COROSIO_FAMILY_HPP

namespace boost::corosio {

/** The address family of an IP socket or address.

    This is the portable spelling of the address family throughout
    the public API: `open()` creates a socket in a given family,
    `ip_address::family()` reports the family of an address, and
    socket options receive it when they are applied. The native
    `AF_*` constants appear only at the native boundary.

    A socket's family is fixed when it is opened, but socket option
    types are constructed without a socket in hand. Options therefore
    receive the family at application time: `set_option` and
    `get_option` pass the socket's family to the option's accessors,
    and family-sensitive options select the matching protocol level
    and wire representation. Options that mean the same thing in
    every family ignore the argument.

    Sockets that are not IP sockets pass `v4` to their options; the
    options applicable to them are family-neutral, so the value is
    inert.
*/
enum class family
{
    v4, ///< IPv4 (`AF_INET`)
    v6  ///< IPv6 (`AF_INET6`)
};

} // namespace boost::corosio

#endif
