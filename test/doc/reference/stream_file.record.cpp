//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/stream_file.hpp's
// documentation for stream_file, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what
// the reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/file_base.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/stream_file.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

// tag::stream_file[]
// read_some has an implicit position: it advances automatically after
// each call, unlike random_access_file's explicit offset.
capy::task<>
read_a_file_until_eof(corosio::io_context& ioc)
{
    corosio::stream_file f(ioc);
    if (auto ec = f.open("data.bin", corosio::file_base::read_only))
        co_return; // report the error

    char buf[4096];
    for (;;)
    {
        auto [ec, n] =
            co_await f.read_some(capy::mutable_buffer(buf, sizeof(buf)));
        if (ec == capy::cond::eof)
            break;
        if (ec)
            co_return;
    }
}
// end::stream_file[]

} // namespace
