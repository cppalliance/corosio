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
#include <boost/capy/async_run.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/error.hpp>
#include <boost/url/ipv4_address.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

// Coroutine that performs the HTTP GET request
capy::task<void>
do_request(
    corosio::io_context& ioc,
    boost::urls::ipv4_address addr,
    std::uint16_t port)
{
    corosio::socket s(ioc);
    s.open();

    // Connect to the server
    auto ec = co_await s.connect(
        corosio::endpoint(addr, port));
    if (ec)
    {
        std::cerr << "Connect error: " << ec.message() << "\n";
        co_return;
    }

    // Build the HTTP request
    std::string request =
        "GET / HTTP/1.1\r\n"
        "Host: " + addr.to_string() + "\r\n"
        "Connection: close\r\n"
        "\r\n";

    // Send the request
    auto [write_ec, bytes_written] = co_await corosio::write(
        s, capy::const_buffer(request.data(), request.size()));
    if (write_ec)
    {
        std::cerr << "Write error: " << write_ec.message() << "\n";
        co_return;
    }

    // Read the entire response into a string
    std::string response;
    auto [read_ec, n] = co_await corosio::read(s, response);
    if (read_ec && read_ec != capy::error::eof)
    {
        std::cerr << "Read error: " << read_ec.message() << "\n";
        co_return;
    }

    std::cout << response << std::endl;
}

int
main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr <<
            "Usage: http_client <ip-address> <port>\n"
            "Example:\n"
            "    http_client 35.190.118.110 80\n";
        return EXIT_FAILURE;
    }

    // Parse IP address
    auto addr_result = boost::urls::parse_ipv4_address(argv[1]);
    if (!addr_result)
    {
        std::cerr << "Invalid IP address: " << argv[1] << "\n";
        return EXIT_FAILURE;
    }

    // Parse port
    int port_int = std::atoi(argv[2]);
    if (port_int <= 0 || port_int > 65535)
    {
        std::cerr << "Invalid port: " << argv[2] << "\n";
        return EXIT_FAILURE;
    }
    auto port = static_cast<std::uint16_t>(port_int);

    // Create I/O context and run
    corosio::io_context ioc;
    capy::async_run(ioc.get_executor())(
        do_request(ioc, *addr_result, port));
    ioc.run();

    return EXIT_SUCCESS;
}
