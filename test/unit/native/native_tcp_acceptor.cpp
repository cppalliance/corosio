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
            decltype(std::declval<native_tcp_acceptor<Backend>&>().wait(
                wait_type::read)),
            decltype(std::declval<tcp_acceptor&>().wait(wait_type::read))>,
        "native_tcp_acceptor::wait must shadow tcp_acceptor::wait");

    void testAcceptorConstruct()
    {
        io_context ctx(Backend);
        native_tcp_acceptor<Backend> acc(ctx);
        BOOST_TEST_PASS();
    }

    void testAcceptorMoveConstruct()
    {
        io_context ctx(Backend);
        native_tcp_acceptor<Backend> a1(ctx);
        a1.open();
        a1.set_option(native_socket_option::reuse_address(true));
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
        io_context ctx(Backend);
        native_tcp_acceptor<Backend> na(ctx);
        na.open();
        na.set_option(native_socket_option::reuse_address(true));
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
        io_context ioc(Backend);
        auto       ex = ioc.get_executor();

        native_tcp_acceptor<Backend> acc(ioc);
        acc.open();
        acc.set_option(native_socket_option::reuse_address(true));
        auto bec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);
        auto lec = acc.listen();
        BOOST_TEST(!lec);
        auto port = acc.local_endpoint().port();

        native_tcp_socket<Backend> client(ioc);
        client.open();

        std::error_code wait_ec;
        bool            wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto connector = [&]() -> capy::task<> {
            auto [ec] = co_await client.connect(
                endpoint(ipv4_address::loopback(), port));
            (void)ec;
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(connector());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

#ifdef SO_REUSEPORT
    void testNativeReusePort()
    {
        io_context ctx(Backend);
        native_tcp_acceptor<Backend> acc(ctx);
        acc.open();

        acc.set_option(native_socket_option::reuse_address(true));
        acc.set_option(native_socket_option::reuse_port(true));
        auto rp =
            acc.template get_option<native_socket_option::reuse_port>();
        BOOST_TEST(rp.value());

        acc.close();
    }
#endif

    void run()
    {
        testAcceptorConstruct();
        testAcceptorMoveConstruct();
        testAcceptorPolymorphicSlice();
        testWait();
#ifdef SO_REUSEPORT
        testNativeReusePort();
#endif
    }
};

#if BOOST_COROSIO_HAS_EPOLL
struct native_tcp_acceptor_test_epoll : native_tcp_acceptor_test<epoll>
{};
TEST_SUITE(
    native_tcp_acceptor_test_epoll, "boost.corosio.native.tcp_acceptor.epoll");
#endif

#if BOOST_COROSIO_HAS_SELECT
struct native_tcp_acceptor_test_select : native_tcp_acceptor_test<select>
{};
TEST_SUITE(
    native_tcp_acceptor_test_select,
    "boost.corosio.native.tcp_acceptor.select");
#endif

#if BOOST_COROSIO_HAS_KQUEUE
struct native_tcp_acceptor_test_kqueue : native_tcp_acceptor_test<kqueue>
{};
TEST_SUITE(
    native_tcp_acceptor_test_kqueue,
    "boost.corosio.native.tcp_acceptor.kqueue");
#endif

#if BOOST_COROSIO_HAS_IOCP
struct native_tcp_acceptor_test_iocp : native_tcp_acceptor_test<iocp>
{};
TEST_SUITE(
    native_tcp_acceptor_test_iocp, "boost.corosio.native.tcp_acceptor.iocp");
#endif

} // namespace boost::corosio
