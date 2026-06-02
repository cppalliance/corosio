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
#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <system_error>
#include <type_traits>
#include <utility>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_tcp_socket_test
{
    // Shadow-engagement checks: the native overload must return a
    // distinct awaitable type from the base. If a shadow is broken
    // (e.g. signature drift, missing override), these fail at compile
    // time. The check is unevaluated; no runtime cost.
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_tcp_socket<Backend>&>().read_some(
                std::declval<capy::mutable_buffer>())),
            decltype(std::declval<io_stream&>().read_some(
                std::declval<capy::mutable_buffer>()))>,
        "native_tcp_socket::read_some must shadow io_stream::read_some");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_tcp_socket<Backend>&>().write_some(
                std::declval<capy::const_buffer>())),
            decltype(std::declval<io_stream&>().write_some(
                std::declval<capy::const_buffer>()))>,
        "native_tcp_socket::write_some must shadow io_stream::write_some");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_tcp_socket<Backend>&>().connect(
                std::declval<endpoint>())),
            decltype(std::declval<tcp_socket&>().connect(
                std::declval<endpoint>()))>,
        "native_tcp_socket::connect must shadow tcp_socket::connect");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_tcp_socket<Backend>&>().wait(
                wait_type::read)),
            decltype(std::declval<tcp_socket&>().wait(wait_type::read))>,
        "native_tcp_socket::wait must shadow tcp_socket::wait");

    void testSocketConstruct()
    {
        io_context ctx(Backend);
        native_tcp_socket<Backend> s(ctx);
        BOOST_TEST_PASS();
    }

    void testSocketMoveConstruct()
    {
        io_context ctx(Backend);
        native_tcp_socket<Backend> s1(ctx);
        s1.open();
        BOOST_TEST(s1.is_open());

        native_tcp_socket<Backend> s2(std::move(s1));
        BOOST_TEST(s2.is_open());
    }

    void testSocketPolymorphicSlice()
    {
        io_context ctx(Backend);
        native_tcp_socket<Backend> ns(ctx);
        ns.open();

        tcp_socket& base = ns;
        BOOST_TEST(base.is_open());

        io_stream& stream_base = ns;
        (void)stream_base;

        io_read_stream& read_base = ns;
        (void)read_base;

        io_write_stream& write_base = ns;
        (void)write_base;

        BOOST_TEST_PASS();
    }

    // Exercise the shadowed wait() awaitable. On a connected socket
    // wait_type::write resolves immediately on every backend (IOCP
    // matches asio's "writable is always ready" semantics).
    void testWait()
    {
        io_context ioc(Backend);
        auto [s1, s2] = test::make_socket_pair<
            native_tcp_socket<Backend>,
            native_tcp_acceptor<Backend>>(ioc);

        std::error_code wait_ec;
        bool            wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::write);
            wait_ec   = ec;
            wait_done = true;
        };
        capy::run_async(ioc.get_executor())(waiter());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    // Exercise inline native_socket_option types on a TCP socket so the
    // boolean<IPPROTO_TCP, TCP_NODELAY> instantiation is fully hit.
    void testNativeNoDelay()
    {
        io_context ctx(Backend);
        native_tcp_socket<Backend> s(ctx);
        s.open();

        s.set_option(native_socket_option::no_delay(true));
        auto nd = s.template get_option<native_socket_option::no_delay>();
        BOOST_TEST(nd.value());

        s.set_option(native_socket_option::no_delay(false));
        nd = s.template get_option<native_socket_option::no_delay>();
        BOOST_TEST(!nd.value());

        // Cover member accessors on the inline boolean<>.
        native_socket_option::no_delay direct(true);
        BOOST_TEST(direct.value());
        BOOST_TEST_EQ(direct.size(), sizeof(int));
        BOOST_TEST(direct.data() != nullptr);
        BOOST_TEST_EQ(direct.level(), IPPROTO_TCP);
        BOOST_TEST_EQ(direct.name(), TCP_NODELAY);

        native_socket_option::no_delay const& cd = direct;
        BOOST_TEST(cd.data() != nullptr);

        s.close();
    }

    void run()
    {
        testSocketConstruct();
        testSocketMoveConstruct();
        testSocketPolymorphicSlice();
        testWait();
        testNativeNoDelay();
    }
};

COROSIO_BACKEND_TESTS(native_tcp_socket_test, "boost.corosio.native.tcp_socket")

} // namespace boost::corosio
