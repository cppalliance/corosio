//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/io/io_stream.hpp's
// documentation for io_stream, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what
// the reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/io/io_stream.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/task.hpp>

#include <cstddef>
#include <span>
#include <system_error>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

// tag::io_stream[]
// Read until buffer full or EOF
capy::task<>
read_all(corosio::io_stream& stream, std::span<char> buf)
{
    std::size_t total = 0;
    while (total < buf.size())
    {
        auto [ec, n] = co_await stream.read_some(
            capy::mutable_buffer(buf.data() + total, buf.size() - total));
        if (ec == capy::cond::eof)
            break;
        if (ec)
            throw std::system_error(ec);
        total += n;
    }
}
// end::io_stream[]

} // namespace
