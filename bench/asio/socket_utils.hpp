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

#include <utility>

namespace asio_bench {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

/** Create a connected pair of TCP sockets for benchmarking. */
inline std::pair<tcp::socket, tcp::socket> make_socket_pair( asio::io_context& ioc )
{
    tcp::acceptor acceptor( ioc, tcp::endpoint( tcp::v4(), 0 ) );
    acceptor.set_option( tcp::acceptor::reuse_address( true ) );

    tcp::socket client( ioc );
    tcp::socket server( ioc );

    auto endpoint = acceptor.local_endpoint();
    client.connect( tcp::endpoint( asio::ip::address_v4::loopback(), endpoint.port() ) );
    server = acceptor.accept();

    client.set_option( tcp::no_delay( true ) );
    server.set_option( tcp::no_delay( true ) );

    return { std::move( client ), std::move( server ) };
}

} // namespace asio_bench

#endif
