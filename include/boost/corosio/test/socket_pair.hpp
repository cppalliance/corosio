//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_SOCKET_PAIR_HPP
#define BOOST_COROSIO_TEST_SOCKET_PAIR_HPP

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstdio>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace boost::corosio::test {

/** Create a connected pair of sockets.

    Creates two sockets connected via loopback TCP sockets.
    Data written to one socket can be read from the other.

    @tparam Socket The socket type (default `tcp_socket`).
    @tparam Acceptor The acceptor type (default `tcp_acceptor`).

    @param ctx The I/O context for the sockets.

    @return A pair of connected sockets.
*/
template<class Socket = tcp_socket, class Acceptor = tcp_acceptor>
std::pair<Socket, Socket>
make_socket_pair(io_context& ctx)
{
    auto ex = ctx.get_executor();

    std::error_code accept_ec;
    std::error_code connect_ec;
    bool accept_done  = false;
    bool connect_done = false;

    Acceptor acc(ctx);
    if (auto ec = acc.listen(endpoint(ipv4_address::loopback(), 0)))
        throw std::runtime_error("socket_pair listen failed: " + ec.message());
    auto port = acc.local_endpoint().port();

    Socket s1(ctx);
    Socket s2(ctx);
    s2.open();

    capy::run_async(ex)(
        [](Acceptor& a, Socket& s, std::error_code& ec_out,
           bool& done_out) -> capy::task<> {
            auto [ec] = co_await a.accept(s);
            ec_out    = ec;
            done_out  = true;
        }(acc, s1, accept_ec, accept_done));

    capy::run_async(ex)(
        [](Socket& s, endpoint ep, std::error_code& ec_out,
           bool& done_out) -> capy::task<> {
            auto [ec] = co_await s.connect(ep);
            ec_out    = ec;
            done_out  = true;
        }(s2, endpoint(ipv4_address::loopback(), port), connect_ec,
                           connect_done));

    ctx.run();
    ctx.restart();

    if (!accept_done || accept_ec)
    {
        std::fprintf(
            stderr, "socket_pair: accept failed (done=%d, ec=%s)\n",
            accept_done, accept_ec.message().c_str());
        acc.close();
        throw std::runtime_error("socket_pair accept failed");
    }

    if (!connect_done || connect_ec)
    {
        std::fprintf(
            stderr, "socket_pair: connect failed (done=%d, ec=%s)\n",
            connect_done, connect_ec.message().c_str());
        acc.close();
        s1.close();
        throw std::runtime_error("socket_pair connect failed");
    }

    acc.close();

    s1.set_linger(true, 0);
    s2.set_linger(true, 0);

    return {std::move(s1), std::move(s2)};
}

} // namespace boost::corosio::test

#endif
