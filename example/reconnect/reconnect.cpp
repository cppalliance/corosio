//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stop_token>
#include <thread>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

/** Exponential backoff delay sequence.

    Produces a series of increasing delays starting from an
    initial value and doubling on each call to @ref next, capped
    at a configured maximum. Call @ref reset to restart the
    sequence after a successful connection.

    @par Example
    @code
    exponential_backoff backoff(500ms, 30s);
    timer.expires_after(backoff.next()); // 500ms
    timer.expires_after(backoff.next()); // 1000ms
    timer.expires_after(backoff.next()); // 2000ms
    backoff.reset();
    timer.expires_after(backoff.next()); // 500ms
    @endcode
*/
struct exponential_backoff
{
    using duration = std::chrono::milliseconds;

private:
    duration initial_;
    duration delay_;
    duration max_;

public:
    /// Construct a backoff with the given initial and maximum delays.
    exponential_backoff(duration initial, duration max) noexcept
        : initial_(initial)
        , delay_(initial)
        , max_(max)
    {
    }

    /// Return the current delay and advance to the next.
    duration next() noexcept
    {
        auto current = (std::min)(delay_, max_);
        delay_       = (std::min)(delay_ * 2, max_);
        return current;
    }

    /// Restart the sequence from the initial delay.
    void reset() noexcept
    {
        delay_ = initial_;
    }
};

/// Read from the socket until the peer disconnects.
capy::task<>
do_session(corosio::tcp_socket& sock)
{
    char buf[4096];
    for (;;)
    {
        auto [ec, n] =
            co_await sock.read_some(capy::mutable_buffer(buf, sizeof buf));
        if (ec)
            break;
        std::cout.write(buf, static_cast<std::streamsize>(n));
        std::cout.flush();
    }
}

/** Connect to an endpoint with exponential backoff.

    Attempts to connect to @p ep, doubling the delay between
    each retry. If the connection cannot be established before
    @p max_attempts are exhausted, the coroutine returns. On a
    successful connection, the session runs until the peer
    disconnects, then reconnection resumes from the initial
    delay.

    The backoff timer is sensitive to the coroutine's stop
    token. Cancelling the token (or calling `io_context::stop()`)
    will end the retry loop cleanly.

    @param ioc The I/O context to use for socket and timer
        operations.
    @param ep The endpoint to connect to.
    @param backoff The backoff policy to use between retries.
    @param max_attempts Maximum connection attempts before
        giving up. Zero means unlimited.
*/
capy::task<>
connect_with_backoff(
    corosio::io_context& ioc,
    corosio::endpoint ep,
    exponential_backoff backoff,
    int max_attempts)
{
    corosio::tcp_socket sock(ioc);
    corosio::timer delay(ioc);
    int attempt = 0;

    for (;;)
    {
        ++attempt;

        auto [ec] = co_await sock.connect(ep);
        if (!ec)
        {
            std::cout << "Connected on attempt " << attempt << std::endl;
            co_await do_session(sock);

            // Peer disconnected — restart the retry sequence
            sock.close();
            backoff.reset();
            attempt = 0;
            std::cout << "Disconnected, reconnecting..." << std::endl;
            continue;
        }

        sock.close();

        std::cout << "Attempt " << attempt << " failed: " << ec.message()
                  << std::endl;

        if (max_attempts > 0 && attempt >= max_attempts)
        {
            std::cout << "Giving up after " << attempt << " attempts"
                      << std::endl;
            co_return;
        }

        auto wait_for = backoff.next();

        std::cout << "Retrying in " << wait_for.count() << "ms" << std::endl;

        delay.expires_after(wait_for);
        auto [timer_ec] = co_await delay.wait();
        if (timer_ec == capy::cond::canceled)
        {
            std::cout << "Retry cancelled" << std::endl;
            co_return;
        }
    }
}

int
main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: reconnect <ip-address> <port>\n"
                     "Example:\n"
                     "    reconnect 127.0.0.1 8080\n";
        return EXIT_FAILURE;
    }

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
    corosio::io_context ioc;

    using namespace std::chrono_literals;
    exponential_backoff backoff(500ms, 30s);

    // The stop_source lets us cancel the coroutine gracefully
    // from any thread. When signaled, pending timer and connect
    // operations return cond::canceled, the coroutine's own loop
    // breaks, and it unwinds through normal control flow.
    //
    // Contrast with io_context::stop(), which yanks the event
    // loop out from under suspended coroutines without giving
    // them a chance to observe cancellation. stop() is safe
    // (pending operations are cleaned up during destruction),
    // but coroutine-internal cleanup logic is bypassed.
    std::stop_source stop_src;

    capy::run_async(ioc.get_executor(), stop_src.get_token())(
        connect_with_backoff(ioc, corosio::endpoint(addr, port), backoff, 10));

    // Run the event loop on a background thread so main
    // can signal cancellation after a timeout.
    std::thread worker([&ioc] { ioc.run(); });

    std::this_thread::sleep_for(5s);
    stop_src.request_stop();
    worker.join();

    return EXIT_SUCCESS;
}
