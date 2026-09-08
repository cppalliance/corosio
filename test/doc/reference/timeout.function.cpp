//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/timeout.hpp's
// documentation for timeout, by doc/addons/extensions/reference-snippets.lua.
// The tagged region is what the reference renders; scaffolding stays outside
// the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/timeout.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <iostream>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

// tag::timeout[]
// Precondition: sock is an open, connected socket.
capy::task<>
read_with_deadline(corosio::tcp_socket& sock, capy::mutable_buffer buf)
{
    auto [ec, n] = co_await corosio::timeout(
        sock.read_some(buf), std::chrono::milliseconds(50));

    if (ec == capy::cond::timeout)
        std::cout << "No data within 50ms\n";
    else if (ec == capy::cond::canceled)
        std::cout << "Read was cancelled\n";
    else if (!ec)
        std::cout << "Read " << n << " bytes\n";
    else
        std::cout << "Read failed: " << ec.message() << "\n";
}
// end::timeout[]

} // namespace
