//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tcp_acceptor.hpp's
// documentation for tcp_acceptor::get_option, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_acceptor.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::get_option[]
// Precondition: acc is open (get_option throws bad_file_descriptor
// otherwise).
bool
reuse_address_is_enabled(corosio::tcp_acceptor& acc)
{
    auto opt = acc.get_option<corosio::socket_option::reuse_address>();
    return opt.value();
}
// end::get_option[]

} // namespace
