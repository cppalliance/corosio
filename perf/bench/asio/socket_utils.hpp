//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef ASIO_BENCH_SOCKET_UTILS_HPP
#define ASIO_BENCH_SOCKET_UTILS_HPP

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <utility>

namespace asio_bench {

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;

// Concrete (non-type-erased) executor types avoid any_io_executor overhead
using executor_type = asio::io_context::executor_type;
using tcp_socket    = asio::basic_stream_socket<tcp, executor_type>;
using tcp_acceptor  = asio::basic_socket_acceptor<tcp, executor_type>;
using timer_type    = asio::basic_waitable_timer<
       std::chrono::steady_clock,
       asio::wait_traits<std::chrono::steady_clock>,
       executor_type>;

/** Create a connected pair of TCP sockets for benchmarking. */
inline std::pair<tcp_socket, tcp_socket>
make_socket_pair(asio::io_context& ioc)
{
    tcp_acceptor acceptor(
        ioc.get_executor(), tcp::endpoint(tcp::v4(), 0),
        true /* reuse_address */);

    tcp_socket client(ioc.get_executor());
    tcp_socket server(ioc.get_executor());

    auto endpoint = acceptor.local_endpoint();
    client.connect(
        tcp::endpoint(asio::ip::address_v4::loopback(), endpoint.port()));
    server = acceptor.accept();

    client.set_option(tcp::no_delay(true));
    server.set_option(tcp::no_delay(true));
    client.set_option(asio::socket_base::linger(true, 0));
    server.set_option(asio::socket_base::linger(true, 0));

    return {std::move(client), std::move(server)};
}

} // namespace asio_bench

#endif
