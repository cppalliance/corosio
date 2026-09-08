//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into
// include/boost/corosio/native/native_io_context.hpp's documentation for
// native_io_context, by doc/addons/extensions/reference-snippets.lua. The
// tagged region is what the reference renders; scaffolding stays outside the
// tags.
//
// native_io_context is a class template (`template<auto Backend>`); the
// reference slug drops the template parameter, but the example must still
// name a concrete backend tag. corosio::epoll is what this library actually
// offers as a compile-time tag on Linux (see backend.hpp); other platforms
// get iocp_t/kqueue_t/select_t/uring_t instead, so the whole example is
// guarded on the tag it names actually existing.

#include "../doc_warnings.hpp"

#include <boost/corosio/backend.hpp>
#include <boost/corosio/native/native_io_context.hpp>

namespace corosio = boost::corosio;

namespace {

#if BOOST_COROSIO_HAS_EPOLL
// tag::poll[]
void
poll_native_context()
{
    corosio::native_io_context<corosio::epoll> ctx;
    ctx.poll(); // devirtualized call, no vtable dispatch
}
// end::poll[]
#endif // BOOST_COROSIO_HAS_EPOLL

} // namespace
