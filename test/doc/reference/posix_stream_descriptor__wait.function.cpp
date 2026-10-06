//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/posix_stream_descriptor.hpp's
// documentation for posix_stream_descriptor::wait, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.
//
// posix_stream_descriptor's whole class body is wrapped in #if BOOST_COROSIO_POSIX in
// its own header, so the region that names the type is guarded the same way,
// following local_datagram_socket.record.cpp. The includes below are safe
// unconditionally -- the header itself resolves to nothing off POSIX.

#include "../doc_warnings.hpp"

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/posix_stream_descriptor.hpp>

#include <boost/capy/task.hpp>

#if BOOST_COROSIO_POSIX
#include <unistd.h>
#endif

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

#if BOOST_COROSIO_POSIX
// tag::wait[]
capy::task<std::error_code>
await_readable(corosio::io_context& ioc, int fd)
{
    // Adopt a duplicate: assign() takes ownership of the copy, so the
    // caller's own fd stays independent of this object's lifetime.
    int copy = ::dup(fd);
    if (copy < 0)
        co_return std::error_code(errno, std::system_category());

    corosio::posix_stream_descriptor d(ioc);
    if (auto ec = d.assign(copy))
    {
        // A rejected descriptor stays the caller's to close.
        ::close(copy);
        co_return ec;
    }

    auto [ec] = co_await d.wait(corosio::wait_type::read);
    co_return ec;
}
// end::wait[]
#endif

} // namespace
