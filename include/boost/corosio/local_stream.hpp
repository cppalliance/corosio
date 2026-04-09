//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_LOCAL_STREAM_HPP
#define BOOST_COROSIO_LOCAL_STREAM_HPP

#include <boost/corosio/detail/config.hpp>

namespace boost::corosio {

class local_stream_socket;
class local_stream_acceptor;

/** Encapsulate the Unix stream protocol for socket creation.

    This class identifies the Unix domain stream protocol. It is
    used to parameterize socket and acceptor `open()` calls with a
    self-documenting type.

    The `family()`, `type()`, and `protocol()` members are
    implemented in the compiled library to avoid exposing
    platform socket headers. For an inline variant that includes
    platform headers, use @ref native_local_stream.

    @par Preconditions
    Unix domain sockets are POSIX-only. This protocol is
    meaningful only on platforms that support Unix domain sockets.

    @see native_local_stream, local_stream_socket, local_stream_acceptor
*/
class BOOST_COROSIO_DECL local_stream
{
public:
    /// Return the Unix domain address family.
    static int family() noexcept;

    /// Return the stream socket type.
    static int type() noexcept;

    /// Return the protocol number (default 0 for Unix domain).
    static int protocol() noexcept;

    /// The associated socket type.
    using socket = local_stream_socket;

    /// The associated acceptor type.
    using acceptor = local_stream_acceptor;
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_LOCAL_STREAM_HPP
