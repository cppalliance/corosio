//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/io_context.hpp's
// documentation for io_context, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.
//
// The explicit-backend constructor takes any backend tag value; the example
// names corosio::epoll for concreteness. That tag exists only where the
// platform has epoll (see backend.hpp), so the whole region is guarded --
// this file is compiled on every CI leg, including macOS and Windows.
// Injection is textual and reads the tag unconditionally, so the guard has
// no effect on the rendered page.

#include "../doc_warnings.hpp"

#include <boost/corosio/backend.hpp>
#include <boost/corosio/io_context.hpp>

namespace corosio = boost::corosio;

namespace {

#if BOOST_COROSIO_HAS_EPOLL
// tag::construct[]
void
construct_contexts()
{
    corosio::io_context ioc; // platform default (epoll on Linux)
    corosio::io_context ioc2(corosio::epoll); // explicit backend
}
// end::construct[]
#endif // BOOST_COROSIO_HAS_EPOLL

} // namespace
