//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into
// include/boost/corosio/native/native_posix_descriptor.hpp's documentation
// for native_posix_descriptor, by doc/addons/extensions/reference-snippets.lua.
// The tagged region is what the reference renders; scaffolding stays outside
// the tags.
//
// native_posix_descriptor is a class template (`template<auto Backend>`);
// the reference slug drops the template parameter, but the example must
// still name a concrete backend tag. corosio::epoll is what this library
// actually offers as a compile-time tag on Linux (see backend.hpp); every
// backend tag defines descriptor_type, so the type itself is not the
// constraint -- the tag's own existence is. Guarding on
// BOOST_COROSIO_HAS_EPOLL (rather than BOOST_COROSIO_POSIX) matches
// native_local_stream_socket.record.cpp's precedent for this exact class
// of example.

#include "../doc_warnings.hpp"

#include <boost/corosio/backend.hpp>
#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/native/native_posix_descriptor.hpp>

#include <boost/capy/task.hpp>

#if BOOST_COROSIO_HAS_EPOLL
#include <unistd.h>
#endif

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

#if BOOST_COROSIO_HAS_EPOLL
// tag::assign_and_wait[]
capy::task<std::error_code>
await_readable_native(int fd)
{
    corosio::native_io_context<corosio::epoll> ctx;
    corosio::native_posix_descriptor<corosio::epoll> d(ctx);

    // Adopt a duplicate: assign() takes ownership, and the caller's fd
    // must outlive it.
    int copy = ::dup(fd);
    if (copy < 0)
        co_return std::error_code(errno, std::system_category());
    if (auto ec = d.assign(copy))
    {
        // A rejected descriptor stays the caller's to close.
        ::close(copy);
        co_return ec;
    }

    auto [ec] = co_await d.wait(corosio::wait_type::read);
    co_return ec;
}
// end::assign_and_wait[]
#endif // BOOST_COROSIO_HAS_EPOLL

} // namespace
