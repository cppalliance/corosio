//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/run.hpp>
#include <boost/capy/ex/thread_pool.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

/// Compute FNV-1a hash on the thread pool.
capy::task<std::uint64_t>
compute_fnv1a( char const* data, std::size_t len )
{
    constexpr std::uint64_t basis = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;

    std::uint64_t h = basis;
    for (std::size_t i = 0; i < len; ++i)
    {
        h ^= static_cast<unsigned char>( data[i] );
        h *= prime;
    }
    co_return h;
}

/// Format a 64-bit value as 16 lowercase hex characters.
std::string
to_hex( std::uint64_t v )
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string s( 16, '0' );
    for (int i = 15; i >= 0; --i)
    {
        s[i] = digits[v & 0xf];
        v >>= 4;
    }
    return s;
}

/// Handle a single client connection.
capy::task<>
do_session(
    corosio::tcp_socket sock,
    capy::thread_pool& pool )
{
    char buf[4096];

    // Read data from client (on io_context)
    auto [ec, n] = co_await sock.read_some(
        capy::mutable_buffer( buf, sizeof( buf ) ) );

    if (ec)
    {
        sock.close();
        co_return;
    }

    // Switch to thread pool for CPU-bound hash computation,
    // then automatically resume on io_context when done
    auto hash = co_await capy::run( pool.get_executor() )(
        compute_fnv1a( buf, n ) );

    // Send hex result back to client (on io_context)
    auto result = to_hex( hash ) + "\n";
    auto [wec, wn] = co_await capy::write(
        sock,
        capy::const_buffer( result.data(), result.size() ) );
    (void)wec;
    (void)wn;

    sock.close();
}

/// Accept loop — spawns a session coroutine per connection.
capy::task<>
do_accept(
    corosio::io_context& ioc,
    corosio::tcp_acceptor& acc,
    capy::thread_pool& pool )
{
    for (;;)
    {
        corosio::tcp_socket peer( ioc );
        auto [ec] = co_await acc.accept( peer );
        if (ec)
            break;

        // Fire-and-forget: each session runs independently
        capy::run_async( ioc.get_executor() )(
            do_session( std::move( peer ), pool ) );
    }
}

int
main( int argc, char* argv[] )
{
    if (argc != 2)
    {
        std::cerr <<
            "Usage: hash_server <port>\n"
            "Example:\n"
            "    hash_server 8080\n";
        return EXIT_FAILURE;
    }

    int port_int = std::atoi( argv[1] );
    if (port_int <= 0 || port_int > 65535)
    {
        std::cerr << "Invalid port: " << argv[1] << "\n";
        return EXIT_FAILURE;
    }
    auto port = static_cast<std::uint16_t>( port_int );

    corosio::io_context ioc;
    capy::thread_pool pool( 4 );

    // Convenience ctor: open + SO_REUSEADDR + bind + listen
    corosio::tcp_acceptor acc( ioc, corosio::endpoint( port ) );

    std::cout << "Hash server listening on port " << port << "\n";

    capy::run_async( ioc.get_executor() )(
        do_accept( ioc, acc, pool ) );

    ioc.run();
    pool.join();

    return EXIT_SUCCESS;
}
