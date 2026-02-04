//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/test/socket_pair.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <system_error>
#include <boost/corosio/basic_io_context.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstdio>
#include <stdexcept>

namespace boost::corosio::test {

std::pair<tcp_socket, tcp_socket>
make_socket_pair(basic_io_context& ctx)
{
    auto ex = ctx.get_executor();

    std::error_code accept_ec;
    std::error_code connect_ec;
    bool accept_done = false;
    bool connect_done = false;

    // Use ephemeral port (0) - OS assigns an available port
    tcp_acceptor acc(ctx);
    if (auto ec = acc.listen(endpoint(ipv4_address::loopback(), 0)))
        throw std::runtime_error("socket_pair listen failed: " + ec.message());
    auto port = acc.local_endpoint().port();

    tcp_socket s1(ctx);
    tcp_socket s2(ctx);
    s2.open();

    capy::run_async(ex)(
        [](tcp_acceptor& a, tcp_socket& s,
           std::error_code& ec_out, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await a.accept(s);
            ec_out = ec;
            done_out = true;
        }(acc, s1, accept_ec, accept_done));

    capy::run_async(ex)(
        [](tcp_socket& s, endpoint ep,
           std::error_code& ec_out, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await s.connect(ep);
            ec_out = ec;
            done_out = true;
        }(s2, endpoint(ipv4_address::loopback(), port),
          connect_ec, connect_done));

    ctx.run();
    ctx.restart();

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
