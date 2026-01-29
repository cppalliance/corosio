//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio.hpp>
#include <boost/corosio/tls/wolfssl_stream.hpp>
#include <system_error>
#include <boost/capy/task.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/string_dynamic_buffer.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

// Coroutine that performs the HTTPS GET request
capy::task<void>
do_request(
    corosio::io_stream& stream,
    std::string_view host)
{
    // Build and send the HTTP request
    std::string request =
        "GET / HTTP/1.1\r\n"
        "Host: " + std::string(host) + "\r\n"
        "Connection: close\r\n"
        "\r\n";
    if (auto [ec, n] = co_await capy::write(
            stream, capy::const_buffer(request.data(), request.size())); ec)
        throw std::system_error(ec);

    // Read entire response until EOF
    std::string response;
    auto [ec, n] = co_await capy::read(
            stream, capy::string_dynamic_buffer(&response));
    // EOF is expected when server closes connection
    if (ec && ec != capy::error::eof)
        throw std::system_error(ec);

    std::cout << response << std::endl;
}

// Parent coroutine that creates and connects the socket
capy::task<void>
run_client(
    corosio::io_context& ioc,
    corosio::ipv4_address addr,
    std::uint16_t port,
    std::string_view hostname)
{
    corosio::socket s(ioc);
    s.open();

    // Connect to the server
    if (auto [ec] = co_await s.connect(corosio::endpoint(addr, port)); ec)
        throw std::system_error(ec);

    // Configure TLS context
    corosio::tls::context ctx;
    ctx.set_hostname(hostname);
    if (auto ec = ctx.set_default_verify_paths(); ec)
        throw std::system_error(ec);
    if (auto ec = ctx.set_verify_mode(corosio::tls::verify_mode::peer); ec)
        throw std::system_error(ec);

    // Wrap socket in TLS stream
    corosio::wolfssl_stream secure(s, ctx);

    // Perform TLS handshake
    if (auto [ec] = co_await secure.handshake(corosio::wolfssl_stream::client); ec)
        throw std::system_error(ec);

    co_await do_request(secure, hostname);
}

int
main(int argc, char* argv[])
{
    if (argc < 3 || argc > 4)
    {
        std::cerr <<
            "Usage: https_client <ip-address> <port> [hostname]\n"
            "Example:\n"
            "    https_client 35.190.118.110 443 www.boost.org\n";
        return EXIT_FAILURE;
    }

    // Optional hostname for SNI and Host header (defaults to IP if not provided)
    std::string hostname = (argc == 4) ? argv[3] : argv[1];

    // Parse IP address
    corosio::ipv4_address addr;
    if (auto ec = corosio::parse_ipv4_address(argv[1], addr); ec)
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
    try
    {
        corosio::io_context ioc;
        capy::run_async(ioc.get_executor())(
            run_client(ioc, addr, port, hostname));
        ioc.run();
    }
    catch(std::system_error const& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
    catch(std::exception const& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
