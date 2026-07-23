//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// The landing page's quick example. Compiled and linked only: the
// outcome depends on whether anything listens on port 8080, and a
// listener that accepts but stays silent would block read_some forever.

// tag::full[]
#include <boost/corosio.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <iostream>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

capy::task<void> connect_example(corosio::io_context& ioc)
{
    corosio::tcp_socket s(ioc);
    s.open();

    // Connect using structured bindings
    auto [ec] = co_await s.connect(
        corosio::endpoint(corosio::ipv4_address::loopback(), 8080));

    if (ec)
    {
        std::cerr << "Connect failed: " << ec.message() << "\n";
        co_return;
    }

    // Read some data
    char buf[1024];
    auto [read_ec, n] = co_await s.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));

    if (!read_ec)
        std::cout << "Received " << n << " bytes\n";
}

int main()
{
    corosio::io_context ioc;
    capy::run_async(ioc.get_executor())(connect_example(ioc));
    ioc.run();
}
// end::full[]
