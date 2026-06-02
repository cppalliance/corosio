//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/native/native_tcp_socket.hpp>
#include <boost/corosio/native/native_tcp_acceptor.hpp>
#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/native/native_socket_option.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_io_test
{
    void testNativeIO()
    {
        io_context ctx(Backend);
        auto ex = ctx.get_executor();

        native_tcp_acceptor<Backend> acc(ctx);
        acc.open();
        acc.set_option(native_socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        auto port = acc.local_endpoint().port();

        tcp_socket peer(ctx);
        native_tcp_socket<Backend> client(ctx);
        client.open();

        bool done = false;
        std::error_code io_ec;
        std::size_t bytes = 0;

        auto server_task = [](native_tcp_acceptor<Backend>& a, tcp_socket& p,
                              std::error_code& ec_out) -> capy::task<> {
            auto [ec] = co_await a.accept(p);
            ec_out    = ec;
        };

        auto client_task = [](native_tcp_socket<Backend>& s,
                              std::uint16_t port_val, std::error_code& ec_out,
                              std::size_t& bytes_out,
                              bool& done_out) -> capy::task<> {
            auto [ec] = co_await s.connect(
                endpoint(ipv4_address::loopback(), port_val));
            if (ec)
            {
                ec_out   = ec;
                done_out = true;
                co_return;
            }

            auto [wec, wn] =
                co_await s.write_some(capy::const_buffer("hello", 5));
            ec_out    = wec;
            bytes_out = wn;
            done_out  = true;
        };

        capy::run_async(ex)(server_task(acc, peer, io_ec));
        capy::run_async(ex)(client_task(client, port, io_ec, bytes, done));

        ctx.run();
        BOOST_TEST(done);
        BOOST_TEST(!io_ec);
        BOOST_TEST(bytes == 5u);
    }

    void run()
    {
        testNativeIO();
    }
};

COROSIO_BACKEND_TESTS(native_io_test, "boost.corosio.native.io")

} // namespace boost::corosio
