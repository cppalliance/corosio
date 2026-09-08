//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into
// include/boost/corosio/native/native_random_access_file.hpp's
// documentation for native_random_access_file, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what
// the reference renders; scaffolding stays outside the tags.
//
// native_random_access_file is a class template (`template<auto Backend>`);
// the reference slug drops the template parameter, but the example must
// still name a concrete backend tag. corosio::epoll is what this library
// actually offers as a compile-time tag on Linux (see backend.hpp),
// matching native_io_context.record.cpp's precedent for this exact class
// of example.

#include "../doc_warnings.hpp"

#include <boost/corosio/backend.hpp>
#include <boost/corosio/file_base.hpp>
#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/native/native_random_access_file.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

#if BOOST_COROSIO_HAS_EPOLL
// tag::native_random_access_file[]
capy::task<>
open_and_read_at()
{
    corosio::native_io_context<corosio::epoll> ctx;
    corosio::native_random_access_file<corosio::epoll> f(ctx);
    if (auto ec = f.open("data.bin", corosio::file_base::read_only))
        co_return;

    char buf[4096];
    auto [ec, n] =
        co_await f.read_some_at(0, capy::mutable_buffer(buf, sizeof(buf)));
    if (ec)
        co_return;
}
// end::native_random_access_file[]
#endif // BOOST_COROSIO_HAS_EPOLL

} // namespace
