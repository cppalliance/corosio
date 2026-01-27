//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/test/socket_pair.hpp>
#include <boost/corosio/acceptor.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/url/ipv4_address.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

#ifdef _WIN32
#include <process.h>  // _getpid()
#else
#include <unistd.h>   // getpid()
#endif

namespace boost::corosio::test {

namespace {

// Use atomic for thread safety when tests run in parallel
std::atomic<std::uint16_t> next_test_port{0};

std::uint16_t
get_test_port() noexcept
{
    // Use a wide port range in the dynamic/ephemeral range (49152-65535)
    constexpr std::uint16_t port_base = 49152;
    constexpr std::uint16_t port_range = 16383;  // 49152-65535

    // Include PID to avoid port collisions between parallel test processes.
    // On Windows with SO_REUSEADDR, multiple processes can bind the same port,
    // causing connections to go to the wrong listener ("port stealing").
    // By using different port ranges per process, we avoid this issue.
#ifdef _WIN32
    auto pid = static_cast<std::uint32_t>(_getpid());
#else
    auto pid = static_cast<std::uint32_t>(getpid());
#endif
    // Mix the PID bits to spread processes across the port range
    auto pid_offset = static_cast<std::uint16_t>((pid * 7919) % port_range);

    auto offset = next_test_port.fetch_add(1, std::memory_order_relaxed);
    return static_cast<std::uint16_t>(port_base + ((pid_offset + offset) % port_range));
}

} // namespace

std::pair<socket, socket>
make_socket_pair(io_context& ioc)
{
    auto ex = ioc.get_executor();

    system::error_code accept_ec;
    system::error_code connect_ec;
    bool accept_done = false;
    bool connect_done = false;

    // Try multiple ports in case of conflicts (TIME_WAIT, parallel tests, etc.)
    std::uint16_t port = 0;
    acceptor acc(ioc);
    bool listening = false;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        port = get_test_port();
        try
        {
            acc.listen(endpoint(urls::ipv4_address::loopback(), port));
            listening = true;
            break;
        }
        catch (const system::system_error&)
        {
            // Port in use, try another
            acc.close();
            acc = acceptor(ioc);
        }
    }
    if (!listening)
    {
        std::fprintf(stderr, "socket_pair: failed to find available port after 20 attempts\n");
        throw std::runtime_error("socket_pair: failed to find available port");
    }

    socket s1(ioc);
    socket s2(ioc);
    s2.open();

    capy::run_async(ex)(
        [](acceptor& a, socket& s,
           system::error_code& ec_out, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await a.accept(s);
            ec_out = ec;
            done_out = true;
        }(acc, s1, accept_ec, accept_done));

    capy::run_async(ex)(
        [](socket& s, endpoint ep,
           system::error_code& ec_out, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await s.connect(ep);
            ec_out = ec;
            done_out = true;
        }(s2, endpoint(urls::ipv4_address::loopback(), port),
          connect_ec, connect_done));

    ioc.run();
    ioc.restart();

    if (!accept_done || accept_ec)
    {
        std::fprintf(stderr, "socket_pair: accept failed (done=%d, ec=%s)\n",
            accept_done, accept_ec.message().c_str());
        acc.close();
        throw std::runtime_error("socket_pair accept failed");
    }

    if (!connect_done || connect_ec)
    {
        std::fprintf(stderr, "socket_pair: connect failed (done=%d, ec=%s)\n",
            connect_done, connect_ec.message().c_str());
        acc.close();
        s1.close();
        throw std::runtime_error("socket_pair connect failed");
    }

    acc.close();

    return {std::move(s1), std::move(s2)};
}

} // namespace boost::corosio::test
