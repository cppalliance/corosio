//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/local_connect_pair.hpp>

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_stream_socket.hpp>

#if BOOST_COROSIO_POSIX
#include <boost/corosio/local_datagram_socket.hpp>
#endif

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstring>
#include <system_error>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct local_connect_pair_test
{
    void testStreamPairExchangesData()
    {
        io_context ioc(Backend);
        local_stream_socket a(ioc), b(ioc);

        BOOST_TEST(!connect_pair(a, b));
        BOOST_TEST(a.is_open());
        BOOST_TEST(b.is_open());

        auto ex = ioc.get_executor();

        char const msg[] = "ping";
        char buf[16]     = {};

        std::error_code wec, rec;
        std::size_t     wn = 0, rn = 0;

        capy::run_async(ex)(
            [](local_stream_socket& s, char const* d, std::size_t len,
               std::error_code& ec_out, std::size_t& n_out) -> capy::task<> {
                auto [ec, n] =
                    co_await s.write_some(capy::const_buffer(d, len));
                ec_out = ec;
                n_out  = n;
            }(a, msg, std::strlen(msg), wec, wn));

        capy::run_async(ex)(
            [](local_stream_socket& s, char* d, std::size_t len,
               std::error_code& ec_out, std::size_t& n_out) -> capy::task<> {
                auto [ec, n] =
                    co_await s.read_some(capy::mutable_buffer(d, len));
                ec_out = ec;
                n_out  = n;
            }(b, buf, sizeof(buf), rec, rn));

        ioc.run();

        BOOST_TEST(!wec);
        BOOST_TEST(!rec);
        BOOST_TEST_EQ(wn, std::strlen(msg));
        BOOST_TEST_EQ(rn, std::strlen(msg));
        BOOST_TEST_EQ(std::strncmp(buf, msg, std::strlen(msg)), 0);
    }

    void testStreamPairRejectsOpenSocket()
    {
        io_context ioc(Backend);
        local_stream_socket a(ioc), b(ioc);
        a.open();
        // a is open; connect_pair must refuse and leave both sockets
        // in their original state (a open, b closed).
        auto ec = connect_pair(a, b);
        BOOST_TEST(static_cast<bool>(ec));
        BOOST_TEST(a.is_open());
        BOOST_TEST(!b.is_open());
    }

#if BOOST_COROSIO_POSIX
    void testDatagramPairExchangesData()
    {
        io_context ioc(Backend);
        local_datagram_socket a(ioc), b(ioc);

        BOOST_TEST(!connect_pair(a, b));
        BOOST_TEST(a.is_open());
        BOOST_TEST(b.is_open());

        auto ex = ioc.get_executor();

        char const msg[] = "dgram";
        char buf[16]     = {};

        std::error_code sec, rec;
        std::size_t     sn = 0, rn = 0;

        capy::run_async(ex)(
            [](local_datagram_socket& s, char const* d, std::size_t len,
               std::error_code& ec_out, std::size_t& n_out) -> capy::task<> {
                auto [ec, n] =
                    co_await s.send(capy::const_buffer(d, len));
                ec_out = ec;
                n_out  = n;
            }(a, msg, std::strlen(msg), sec, sn));

        capy::run_async(ex)(
            [](local_datagram_socket& s, char* d, std::size_t len,
               std::error_code& ec_out, std::size_t& n_out) -> capy::task<> {
                auto [ec, n] =
                    co_await s.recv(capy::mutable_buffer(d, len));
                ec_out = ec;
                n_out  = n;
            }(b, buf, sizeof(buf), rec, rn));

        ioc.run();

        BOOST_TEST(!sec);
        BOOST_TEST(!rec);
        BOOST_TEST_EQ(sn, std::strlen(msg));
        BOOST_TEST_EQ(rn, std::strlen(msg));
        BOOST_TEST_EQ(std::strncmp(buf, msg, std::strlen(msg)), 0);
    }
#endif

    void run()
    {
        testStreamPairExchangesData();
        testStreamPairRejectsOpenSocket();
#if BOOST_COROSIO_POSIX
        testDatagramPairExchangesData();
#endif
    }
};

COROSIO_BACKEND_TESTS(local_connect_pair_test, "boost.corosio.local_connect_pair")

} // namespace boost::corosio
