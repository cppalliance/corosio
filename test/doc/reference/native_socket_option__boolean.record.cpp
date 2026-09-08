//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into
// include/boost/corosio/native/native_socket_option.hpp's documentation for
// native_socket_option::boolean, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/native/native_socket_option.hpp>
#include <boost/corosio/tcp_socket.hpp>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace corosio = boost::corosio;

namespace {

// tag::boolean[]
void
receive_urgent_data_inline(corosio::tcp_socket& sock)
{
    // corosio has no dedicated type for SO_OOBINLINE; naming the level and
    // option as template arguments is what this class is for -- reaching an
    // option the library does not wrap, instead of hand-rolling a
    // setsockopt() call.
    using oob_inline =
        corosio::native_socket_option::boolean<SOL_SOCKET, SO_OOBINLINE>;

    // A peer's TCP urgent byte normally has to be read separately with
    // MSG_OOB; enabling this folds it into the regular byte stream at its
    // marked position instead.
    sock.set_option(oob_inline(true));
}
// end::boolean[]

} // namespace
