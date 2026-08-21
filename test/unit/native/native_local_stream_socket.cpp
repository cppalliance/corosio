//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/native/native_local_stream_socket.hpp>

#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/native/native_local_stream_acceptor.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/test/temp_path.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_local_stream_socket_test
{
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_local_stream_socket<Backend>&>()
                         .read_some(std::declval<capy::mutable_buffer>())),
            decltype(std::declval<io_stream&>().read_some(
                std::declval<capy::mutable_buffer>()))>,
        "native_local_stream_socket::read_some must shadow io_stream::read_some");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_local_stream_socket<Backend>&>()
                         .write_some(std::declval<capy::const_buffer>())),
            decltype(std::declval<io_stream&>().write_some(
                std::declval<capy::const_buffer>()))>,
        "native_local_stream_socket::write_some must shadow io_stream::write_some");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_local_stream_socket<Backend>&>()
                         .connect(std::declval<local_endpoint>())),
            decltype(std::declval<local_stream_socket&>().connect(
                std::declval<local_endpoint>()))>,
        "native_local_stream_socket::connect must shadow local_stream_socket::connect");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_local_stream_acceptor<Backend>&>()
                         .accept(std::declval<local_stream_socket&>())),
            decltype(std::declval<local_stream_acceptor&>().accept(
                std::declval<local_stream_socket&>()))>,
        "native_local_stream_acceptor::accept(peer) must shadow local_stream_acceptor::accept(peer)");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_local_stream_acceptor<Backend>&>()
                         .accept()),
            decltype(std::declval<local_stream_acceptor&>().accept())>,
        "native_local_stream_acceptor::accept() must shadow local_stream_acceptor::accept()");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_local_stream_socket<Backend>&>().wait(
                wait_type::read)),
            decltype(std::declval<local_stream_socket&>().wait(
                wait_type::read))>,
        "native_local_stream_socket::wait must shadow local_stream_socket::wait");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_local_stream_acceptor<Backend>&>()
                         .wait(wait_type::read)),
            decltype(std::declval<local_stream_acceptor&>().wait(
                wait_type::read))>,
        "native_local_stream_acceptor::wait must shadow local_stream_acceptor::wait");

    void testConstruct()
    {
        io_context ioc(Backend);
        native_local_stream_socket<Backend> s(ioc);
        BOOST_TEST_EQ(s.is_open(), false);
    }

    void testOpen()
    {
        io_context ioc(Backend);
        native_local_stream_socket<Backend> s(ioc);
        BOOST_TEST(!s.open());
        BOOST_TEST(s.is_open());
        s.close();
        BOOST_TEST_EQ(s.is_open(), false);
    }

    void testPolymorphicSlice()
    {
        io_context ioc(Backend);
        native_local_stream_socket<Backend> s(ioc);
        BOOST_TEST(!s.open());
        local_stream_socket& base = s;
        BOOST_TEST(base.is_open());
    }

    void testConnectAcceptReadWrite()
    {
        io_context ioc(Backend);
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        native_local_stream_acceptor<Backend> acc(ioc);
        BOOST_TEST(!acc.open());
        auto ec = acc.bind(local_endpoint(path));
        BOOST_TEST_EQ(ec, std::error_code{});
        ec = acc.listen();
        BOOST_TEST_EQ(ec, std::error_code{});

        native_local_stream_socket<Backend> server(ioc);
        native_local_stream_socket<Backend> client(ioc);

        auto acceptor_task =
            [](native_local_stream_acceptor<Backend>& a,
               native_local_stream_socket<Backend>& s) -> capy::task<> {
            auto [ec] = co_await a.accept(s);
            BOOST_TEST_EQ(ec, std::error_code{});

            char buf[64] = {};
            auto [rec, n] =
                co_await s.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST_EQ(rec, std::error_code{});
            BOOST_TEST_EQ(std::string(buf, n), std::string("native unix"));
        };

        auto client_task = [](native_local_stream_socket<Backend>& c,
                              local_endpoint ep) -> capy::task<> {
            auto [ec] = co_await c.connect(ep);
            BOOST_TEST_EQ(ec, std::error_code{});

            char const msg[] = "native unix";
            auto [wec, n] =
                co_await c.write_some(capy::const_buffer(msg, sizeof(msg) - 1));
            BOOST_TEST_EQ(wec, std::error_code{});
            BOOST_TEST_EQ(n, sizeof(msg) - 1);
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(acceptor_task(acc, server));
        capy::run_async(ex)(client_task(client, local_endpoint(path)));
        ioc.run();
    }

    void testMoveAccept()
    {
        io_context ioc(Backend);
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        native_local_stream_acceptor<Backend> acc(ioc);
        BOOST_TEST(!acc.open());
        auto ec = acc.bind(local_endpoint(path));
        BOOST_TEST_EQ(ec, std::error_code{});
        ec = acc.listen();
        BOOST_TEST_EQ(ec, std::error_code{});

        native_local_stream_socket<Backend> client(ioc);

        auto acceptor_task =
            [](native_local_stream_acceptor<Backend>& a) -> capy::task<> {
            auto [ec, peer] = co_await a.accept();
            BOOST_TEST_EQ(ec, std::error_code{});
            BOOST_TEST(peer.is_open());

            char buf[64] = {};
            auto [rec, n] =
                co_await peer.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST_EQ(rec, std::error_code{});
            BOOST_TEST_EQ(std::string(buf, n), std::string("move accept"));
        };

        auto client_task = [](native_local_stream_socket<Backend>& c,
                              local_endpoint ep) -> capy::task<> {
            auto [ec] = co_await c.connect(ep);
            BOOST_TEST_EQ(ec, std::error_code{});

            char const msg[] = "move accept";
            auto [wec, n] =
                co_await c.write_some(capy::const_buffer(msg, sizeof(msg) - 1));
            BOOST_TEST_EQ(wec, std::error_code{});
            BOOST_TEST_EQ(n, sizeof(msg) - 1);
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(acceptor_task(acc));
        capy::run_async(ex)(client_task(client, local_endpoint(path)));
        ioc.run();
    }

    void testVirtualDispatchFallback()
    {
        io_context ioc(Backend);
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        native_local_stream_acceptor<Backend> acc(ioc);
        BOOST_TEST(!acc.open());
        auto ec = acc.bind(local_endpoint(path));
        BOOST_TEST_EQ(ec, std::error_code{});
        ec = acc.listen();
        BOOST_TEST_EQ(ec, std::error_code{});

        native_local_stream_socket<Backend> server(ioc);
        native_local_stream_socket<Backend> client(ioc);

        local_stream_acceptor& acc_ref = acc;
        local_stream_socket& server_ref = server;
        local_stream_socket& client_ref = client;

        auto acceptor_task = [](local_stream_acceptor& a,
                                local_stream_socket& s) -> capy::task<> {
            auto [ec] = co_await a.accept(s);
            BOOST_TEST_EQ(ec, std::error_code{});

            char buf[64] = {};
            auto [rec, n] =
                co_await s.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST_EQ(rec, std::error_code{});
            BOOST_TEST_EQ(std::string(buf, n), std::string("virtual"));
        };

        auto client_task = [](local_stream_socket& c,
                              local_endpoint ep) -> capy::task<> {
            auto [ec] = co_await c.connect(ep);
            BOOST_TEST_EQ(ec, std::error_code{});

            char const msg[] = "virtual";
            std::ignore = co_await c.write_some(
                capy::const_buffer(msg, sizeof(msg) - 1));
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(acceptor_task(acc_ref, server_ref));
        capy::run_async(ex)(client_task(client_ref, local_endpoint(path)));
        ioc.run();
    }

    // Exercise the shadowed wait() awaitable on the socket: a
    // connected stream with an empty send buffer probes writable, so
    // wait_type::write resolves without parking on every backend.
    void testSocketWait()
    {
        io_context ioc(Backend);
        auto       ex   = ioc.get_executor();
        test::temp_socket_dir tmp;
        auto       path = tmp.path();

        native_local_stream_acceptor<Backend> acc(ioc);
        BOOST_TEST(!acc.open());
        auto bec = acc.bind(local_endpoint(path));
        BOOST_TEST(!bec);
        auto lec = acc.listen();
        BOOST_TEST(!lec);

        native_local_stream_socket<Backend> server(ioc);
        native_local_stream_socket<Backend> client(ioc);

        std::error_code wait_ec;
        bool            wait_done = false;

        auto rendezvous = [&]() -> capy::task<> {
            [[maybe_unused]] auto [ec] = co_await acc.accept(server);
        };
        auto connect_task = [&]() -> capy::task<> {
            [[maybe_unused]] auto [ec] = co_await client.connect(local_endpoint(path));
        };
        capy::run_async(ex)(rendezvous());
        capy::run_async(ex)(connect_task());
        ioc.run();
        ioc.restart();

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await client.wait(wait_type::write);
            wait_ec   = ec;
            wait_done = true;
        };
        capy::run_async(ex)(waiter());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    // Exercise the shadowed wait() awaitable on the acceptor:
    // wait_type::read resolves once a client connects.
    void testAcceptorWait()
    {
        io_context ioc(Backend);
        auto       ex   = ioc.get_executor();
        test::temp_socket_dir tmp;
        auto       path = tmp.path();

        native_local_stream_acceptor<Backend> acc(ioc);
        BOOST_TEST(!acc.open());
        auto bec = acc.bind(local_endpoint(path));
        BOOST_TEST(!bec);
        auto lec = acc.listen();
        BOOST_TEST(!lec);

        native_local_stream_socket<Backend> client(ioc);

        std::error_code wait_ec;
        bool            wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto connect_task = [&]() -> capy::task<> {
            [[maybe_unused]] auto [ec] = co_await client.connect(local_endpoint(path));
        };
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(connect_task());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    void testMovedFromValueAcceptThrows()
    {
        io_context ioc(Backend);
        native_local_stream_acceptor<Backend> a(ioc);
        native_local_stream_acceptor<Backend> b(std::move(a));

        bool threw = false;
        try
        {
            std::ignore = a.accept();
        }
        catch (std::logic_error const&)
        {
            threw = true;
        }
        BOOST_TEST(threw);
    }

    void testAcceptOnClosedCompletes()
    {
        // Accepts on a closed acceptor complete with
        // bad_file_descriptor instead of throwing.
        io_context ioc(Backend);
        native_local_stream_acceptor<Backend> acc(ioc);
        native_local_stream_socket<Backend> peer(ioc);

        bool done = false;
        auto task = [&]() -> capy::task<> {
            auto [e1] = co_await acc.accept(peer);
            BOOST_TEST(e1 == std::errc::bad_file_descriptor);

            auto [e2, moved] = co_await acc.accept();
            BOOST_TEST(e2 == std::errc::bad_file_descriptor);
            done = true;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(done);
    }

    void testConnectAutoOpens()
    {
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        // temp dir exists, but the socket file inside it does not
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        native_local_stream_socket<Backend> s(ioc);
        BOOST_TEST_EQ(s.is_open(), false);

        std::error_code result_ec;
        bool done = false;

        capy::run_async(ex)(
            [](native_local_stream_socket<Backend>& sock,
               local_endpoint ep,
               std::error_code& ec_out, bool& d) -> capy::task<> {
                auto [ec] = co_await sock.connect(ep);
                ec_out = ec;
                d      = true;
            }(s, local_endpoint(path), result_ec, done));

        ioc.run();
        BOOST_TEST(done);
        BOOST_TEST(s.is_open());
    }

    // A closed acceptor's wait completes with bad_file_descriptor,
    // matching the base-class contract.
    // Stream operations on a closed socket complete with
    // bad_file_descriptor instead of dispatching.
    void testSocketClosedOpsComplete()
    {
        io_context ioc(Backend);
        native_local_stream_socket<Backend> s(ioc);

        bool done = false;
        auto task = [&]() -> capy::task<> {
            char buf[8] = {};

            auto [rec, rn] = co_await s.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(rec == std::errc::bad_file_descriptor);
            BOOST_TEST_EQ(rn, 0u);

            auto [wec, wn] = co_await s.write_some(
                capy::const_buffer(buf, sizeof(buf)));
            BOOST_TEST(wec == std::errc::bad_file_descriptor);
            BOOST_TEST_EQ(wn, 0u);

            auto [tec] = co_await s.wait(wait_type::read);
            BOOST_TEST(tec == std::errc::bad_file_descriptor);
            done = true;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(done);
    }

    void testAcceptorWaitOnClosed()
    {
        io_context ioc(Backend);
        native_local_stream_acceptor<Backend> acc(ioc);

        std::error_code wait_ec;
        bool            wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        capy::run_async(ioc.get_executor())(waiter());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == std::errc::bad_file_descriptor);
    }

    void run()
    {
        testConstruct();
        testOpen();
        testPolymorphicSlice();
        testConnectAcceptReadWrite();
        testMoveAccept();
        testVirtualDispatchFallback();
        testSocketWait();
        testSocketClosedOpsComplete();
        testAcceptorWait();
        testAcceptorWaitOnClosed();
        testAcceptOnClosedCompletes();
        testMovedFromValueAcceptThrows();
        testConnectAutoOpens();
    }
};

COROSIO_BACKEND_TESTS(
    native_local_stream_socket_test, "boost.corosio.native.local_stream_socket")

} // namespace boost::corosio
