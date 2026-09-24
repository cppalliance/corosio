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

/** The IP address family a socket option is rendered for.

    A socket's family is fixed when it is opened, but socket option
    types are constructed without a socket in hand. Options therefore
    receive the family when they are applied: `set_option` passes the
    socket's family to the option's `level`, `name`, `data`, and
    `size` accessors, and family-sensitive options select the
    matching protocol level and wire representation. Options that
    mean the same thing in every family ignore the argument.

    Sockets that are not IP sockets pass `v4`; the options applicable
    to them are family-neutral, so the value is inert.
*/
enum class family
{
    v4, ///< IPv4 (`AF_INET`)
    v6  ///< IPv6 (`AF_INET6`)
};

} // namespace boost::corosio

#endif
