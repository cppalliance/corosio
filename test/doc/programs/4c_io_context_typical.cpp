//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// The io_context page's typical usage pattern: create I/O objects,
// launch the initial coroutine, run until all work completes.

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

// The page focuses on the launch pattern; the coroutine body is a
// placeholder so the program exits cleanly.
capy::task<> main_coroutine(corosio::tcp_socket&)
{
    co_return;
}

// tag::full[]
int main()
{
    corosio::io_context ioc;

    // Create I/O objects
    corosio::tcp_socket sock(ioc);

    // Launch initial coroutine
    capy::run_async(ioc.get_executor())(main_coroutine(sock));

    // Run until all work completes
    ioc.run();
}
// end::full[]
