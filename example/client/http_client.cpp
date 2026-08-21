//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// tag::assume[]
#include <boost/corosio.hpp>
#include <system_error>
#include <boost/capy/task.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>

// end::assume[]
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

// tag::assume[]
namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::assume[]

// tag::build_request[]
std::string build_request(std::string_view host)
{
    return "GET / HTTP/1.1\r\n"
           "Host: " + std::string(host) + "\r\n"
           "Connection: close\r\n"
           "\r\n";
}
// end::build_request[]

// tag::do_request[]
// Coroutine that performs the HTTP GET request
capy::task<void>
do_request(
    corosio::io_stream& stream,
    std::string_view host)
{
    // Build and send the request
    std::string request = build_request(host);
    if (auto [ec, n] = co_await capy::write(
            stream, capy::const_buffer(request.data(), request.size())); ec)
        throw std::system_error(ec);

    // Read the entire response until EOF, one fixed chunk at a time
    // tag::read_loop[]
    std::string response;
    for (;;)
    {
        char chunk[4096];
        auto [ec, n] = co_await capy::read(
            stream, capy::mutable_buffer(chunk, sizeof(chunk)));
        response.append(chunk, n);
        if (ec)
        {
            // EOF is expected when the server closes the connection
            if (ec != capy::error::eof)
                throw std::system_error(ec);
            break;
        }
    }
    // end::read_loop[]

    std::cout << response << std::endl;
}
// end::do_request[]

// tag::run_client[]
// Parent coroutine that creates and connects the socket
capy::task<void>
run_client(
    corosio::io_context& ioc,
    corosio::ipv4_address addr,
    std::uint16_t port)
{
    // connect() opens the socket automatically
    corosio::tcp_socket s(ioc);

    // Connect to the server
    if (auto [ec] = co_await s.connect(corosio::endpoint(addr, port)); ec)
        throw std::system_error(ec);

    co_await do_request(s, addr.to_string());
}
// end::run_client[]

// tag::main[]
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
    auto [aec, addr] = corosio::make_ipv4_address(argv[1]);
    if (aec)
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
    capy::run_async(ioc.get_executor())(
        run_client(ioc, addr, port));
    ioc.run();

    return EXIT_SUCCESS;
}
// end::main[]
