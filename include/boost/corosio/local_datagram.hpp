//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_LOCAL_DATAGRAM_HPP
#define BOOST_COROSIO_LOCAL_DATAGRAM_HPP

#include <boost/corosio/detail/config.hpp>

namespace boost::corosio {

class local_datagram_socket;

/* Encapsulate the Unix datagram protocol for socket creation.

   This class identifies the Unix domain datagram protocol. It is
   used to parameterize socket open() calls with a self-documenting
   type.

   The family(), type(), and protocol() members are implemented
   in the compiled library to avoid exposing platform socket
   headers.

   See local_datagram_socket
*/
class BOOST_COROSIO_DECL local_datagram
{
public:
    /// Return the Unix domain address family.
    static int family() noexcept;

    /// Return the datagram socket type.
    static int type() noexcept;

    /// Return the protocol number (default 0).
    static int protocol() noexcept;

    /// The associated socket type.
    using socket = local_datagram_socket;
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_LOCAL_DATAGRAM_HPP
