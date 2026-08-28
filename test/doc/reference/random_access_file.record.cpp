//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into
// include/boost/corosio/random_access_file.hpp's documentation for
// random_access_file, by doc/addons/extensions/reference-snippets.lua. The
// tagged region is what the reference renders; scaffolding stays outside
// the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/file_base.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/random_access_file.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

// tag::random_access_file[]
// Every read/write names an explicit byte offset; there is no implicit
// position to advance, unlike stream_file.
capy::task<> read_a_file_at_an_offset(corosio::io_context& ioc)
{
    corosio::random_access_file f(ioc);
    if (auto ec = f.open("data.bin", corosio::file_base::read_only))
        co_return;  // report the error

    char buf[4096];
    auto [ec, n] = co_await f.read_some_at(
        0, capy::mutable_buffer(buf, sizeof(buf)));
    if (ec)
        co_return;
}
// end::random_access_file[]

} // namespace
