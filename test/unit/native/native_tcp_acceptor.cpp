//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/native/native_tcp_acceptor.hpp>
#include <boost/corosio/native/native_tcp_socket.hpp>
#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/native/native_socket_option.hpp>

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <system_error>
#include <type_traits>
#include <utility>
#include <tuple>

#include <boost/corosio/detail/platform.hpp>
#if BOOST_COROSIO_POSIX
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "context.hpp"
#include "must.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_tcp_acceptor_test
{
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_tcp_acceptor<Backend>&>().accept(
                std::declval<tcp_socket&>())),
            decltype(std::declval<tcp_acceptor&>().accept(
                std::declval<tcp_socket&>()))>,
        "native_tcp_acceptor::accept must shadow tcp_acceptor::accept");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_tcp_acceptor<Backend>&>().accept()),
            decltype(std::declval<tcp_acceptor&>().accept())>,
        "native_tcp_acceptor::accept() must shadow tcp_acceptor::accept()");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_tcp_acceptor<Backend>&>().wait(
                wait_type::read)),
            decltype(std::declval<tcp_acceptor&>().wait(wait_type::read))>,
        "native_tcp_acceptor::wait must shadow tcp_acceptor::wait");

    void testAcceptorConstruct()
    {
        native_io_context<Backend> ctx;
        native_tcp_acceptor<Backend> acc(ctx);
        BOOST_TEST_PASS();
    }

    void testAcceptorMoveConstruct()
    {
        native_io_context<Backend> ctx;
        native_tcp_acceptor<Backend> a1(ctx);
        BOOST_TEST(!a1.open());
        test::must_ok(a1.set_option(native_socket_option::reuse_address(true)));
        auto ec = a1.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = a1.listen();
        BOOST_TEST(!ec);
        BOOST_TEST(a1.is_open());

        native_tcp_acceptor<Backend> a2(std::move(a1));
        BOOST_TEST(a2.is_open());
    }

    void testAcceptorPolymorphicSlice()
    {
        native_io_context<Backend> ctx;
        native_tcp_acceptor<Backend> na(ctx);
        BOOST_TEST(!na.open());
        test::must_ok(na.set_option(native_socket_option::reuse_address(true)));
        auto ec = na.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = na.listen();
        BOOST_TEST(!ec);

        tcp_acceptor& base = na;
        BOOST_TEST(base.is_open());
    }

    // Exercise the shadowed wait() awaitable: wait_type::read on a
    // listening acceptor resolves when a connection arrives.
    void testWait()
    {
        native_io_context<Backend> ioc;
        auto ex = ioc.get_executor();

        native_tcp_acceptor<Backend> acc(ioc);
        BOOST_TEST(!acc.open());
        test::must_ok(
            acc.set_option(native_socket_option::reuse_address(true)));
        auto bec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);
        auto lec = acc.listen();
        BOOST_TEST(!lec);
        auto port = acc.local_endpoint().port();

        native_tcp_socket<Backend> client(ioc);
        BOOST_TEST(!client.open());

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto connector = [&]() -> capy::task<> {
            [[maybe_unused]] auto [ec] = co_await client.connect(
                endpoint(ipv4_address::loopback(), port));
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(connector());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    // Exercise the shadowed returning accept() awaitable on the
    // devirtualized path: it yields a connected peer socket.
    void testNativeAcceptReturning()
    {
        native_io_context<Backend> ioc;
        auto ex = ioc.get_executor();

        native_tcp_acceptor<Backend> acc(ioc);
        BOOST_TEST(!acc.open());
        test::must_ok(
            acc.set_option(native_socket_option::reuse_address(true)));
        auto bec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);
        auto lec = acc.listen();
        BOOST_TEST(!lec);
        auto port = acc.local_endpoint().port();

        native_tcp_socket<Backend> client(ioc);
        BOOST_TEST(!client.open());

        std::error_code accept_ec;
        bool accept_done    = false;
        bool peer_connected = false;

        auto acceptor = [&]() -> capy::task<> {
            auto [ec, peer] = co_await acc.accept();
            accept_ec       = ec;
            if (!ec)
                peer_connected = peer.remote_endpoint().port() != 0;
            accept_done = true;
        };
        auto connector = [&]() -> capy::task<> {
            [[maybe_unused]] auto [ec] = co_await client.connect(
                endpoint(ipv4_address::loopback(), port));
        };

        capy::run_async(ex)(acceptor());
        capy::run_async(ex)(connector());
        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(peer_connected);
    }

#ifdef SO_REUSEPORT
    void testNativeReusePort()
    {
        native_io_context<Backend> ctx;
        native_tcp_acceptor<Backend> acc(ctx);
        BOOST_TEST(!acc.open());

        test::must_ok(
            acc.set_option(native_socket_option::reuse_address(true)));
        test::must_ok(acc.set_option(native_socket_option::reuse_port(true)));
        auto rp = test::must(
            acc.template get_option<native_socket_option::reuse_port>());
        BOOST_TEST(rp.value());

        acc.close();
    }
#endif

    void testClosedAcceptCompletes()
    {
        // Accepts on a closed (not moved-from) acceptor complete
        // with bad_file_descriptor instead of dispatching.
        native_io_context<Backend> ioc;
        native_tcp_acceptor<Backend> acc(ioc);
        tcp_socket peer(ioc);

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

#if BOOST_COROSIO_POSIX
    // An accept that completed but whose awaiter is destroyed before it
    // resumes must not strand the accepted connection.
    void testAcceptedPeerClosedWhenAwaiterDestroyed(bool by_value)
    {
        native_io_context<Backend> a;
        native_tcp_acceptor<Backend> acc(a);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        auto const port = acc.local_endpoint().port();

        tcp_socket peer(a);
        int client = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(client >= 0);
        {
            native_io_context<Backend> b;
            if (by_value)
                capy::run_async(b.get_executor())(
                    [](native_tcp_acceptor<Backend>& acc) -> capy::task<> {
                        std::ignore = co_await acc.accept();
                    }(acc));
            else
                capy::run_async(b.get_executor())(
                    [](native_tcp_acceptor<Backend>& acc,
                       tcp_socket& p) -> capy::task<> {
                        std::ignore = co_await acc.accept(p);
                    }(acc, peer));
            std::ignore = b.poll(); // parks the accept on a's acceptor

            sockaddr_in addr{};
            addr.sin_family      = AF_INET;
            addr.sin_port        = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            BOOST_TEST(
                ::connect(
                    client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
                0);
            a.run(); // completes the accept into b's queue
        }

        // With the accepted socket closed, the client reads end of file;
        // a stranded one never becomes readable. Some stacks deliver the
        // close asynchronously, so readability is awaited, bounded only
        // to fail rather than hang.
        pollfd pfd{client, POLLIN, 0};
        BOOST_TEST_EQ(::poll(&pfd, 1, 5000), 1);
        ::fcntl(client, F_SETFL, ::fcntl(client, F_GETFL) | O_NONBLOCK);
        char c;
        BOOST_TEST_EQ(::recv(client, &c, 1, 0), 0);
        ::close(client);
    }
#endif

    void testMovedFromValueAccept()
    {
        native_io_context<Backend> ioc;
        native_tcp_acceptor<Backend> a(ioc);
        native_tcp_acceptor<Backend> b(std::move(a));

        bool done = false;
        auto task = [&]() -> capy::task<> {
            auto [ec, peer] = co_await a.accept();
            BOOST_TEST(ec == std::errc::bad_file_descriptor);
            BOOST_TEST(!peer.is_open());
            done = true;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(done);
    }

    // A closed acceptor's wait completes with bad_file_descriptor,
    // matching the base-class contract.
    void testWaitOnClosedAcceptor()
    {
        native_io_context<Backend> ioc;
        native_tcp_acceptor<Backend> acc(ioc);

        std::error_code wait_ec;
        bool wait_done = false;

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
        testAcceptorConstruct();
        testAcceptorMoveConstruct();
        testAcceptorPolymorphicSlice();
        testWait();
        testWaitOnClosedAcceptor();
        testNativeAcceptReturning();
#ifdef SO_REUSEPORT
        testNativeReusePort();
#endif
#if BOOST_COROSIO_POSIX
        testAcceptedPeerClosedWhenAwaiterDestroyed(false);
        testAcceptedPeerClosedWhenAwaiterDestroyed(true);
#endif
        testMovedFromValueAccept();
        testClosedAcceptCompletes();
    }
};

COROSIO_BACKEND_TESTS(
    native_tcp_acceptor_test, "boost.corosio.native.tcp_acceptor")

} // namespace boost::corosio
