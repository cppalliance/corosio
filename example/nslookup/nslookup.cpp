//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/ex/async_run.hpp>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

// Coroutine that performs the DNS lookup
capy::task<void>
do_lookup(
    corosio::io_context& ioc,
    std::string_view host,
    std::string_view service)
{
    corosio::resolver r(ioc);

    auto [ec, results] = co_await r.resolve(host, service);
    if (ec)
    {
        std::cerr << "Resolve failed: " << ec.message() << "\n";
        co_return;
    }

    std::cout << "Results for " << host;
    if (!service.empty())
        std::cout << ":" << service;
    std::cout << "\n";

    for (auto const& entry : results)
    {
        auto ep = entry.get_endpoint();
        if (ep.is_v4())
        {
            std::cout << "  IPv4: " << ep.v4_address().to_string()
                      << ":" << ep.port() << "\n";
        }
        else
        {
            std::cout << "  IPv6: " << ep.v6_address().to_string()
                      << ":" << ep.port() << "\n";
        }
    }

    std::cout << "\nTotal: " << results.size() << " addresses\n";
}

int
main(int argc, char* argv[])
{
    if (argc < 2 || argc > 3)
    {
        std::cerr <<
            "Usage: nslookup <hostname> [service]\n"
            "Examples:\n"
            "    nslookup www.google.com\n"
            "    nslookup www.google.com https\n"
            "    nslookup localhost 8080\n";
        return EXIT_FAILURE;
    }

    std::string_view host = argv[1];
    std::string_view service = (argc == 3) ? argv[2] : "";

    corosio::io_context ioc;
    capy::async_run(ioc.get_executor())(
        do_lookup(ioc, host, service));
    ioc.run();

    return EXIT_SUCCESS;
}
