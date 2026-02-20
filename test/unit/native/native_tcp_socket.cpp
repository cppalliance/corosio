//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/native/native_tcp_socket.hpp>
#include <boost/corosio/native/native_io_context.hpp>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_tcp_socket_test
{
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

    void run()
    {
        testSocketConstruct();
        testSocketMoveConstruct();
        testSocketPolymorphicSlice();
    }
};

#if BOOST_COROSIO_HAS_EPOLL
struct native_tcp_socket_test_epoll : native_tcp_socket_test<epoll>
{};
TEST_SUITE(
    native_tcp_socket_test_epoll, "boost.corosio.native.tcp_socket.epoll");
#endif

#if BOOST_COROSIO_HAS_SELECT
struct native_tcp_socket_test_select : native_tcp_socket_test<select>
{};
TEST_SUITE(
    native_tcp_socket_test_select, "boost.corosio.native.tcp_socket.select");
#endif

#if BOOST_COROSIO_HAS_KQUEUE
struct native_tcp_socket_test_kqueue : native_tcp_socket_test<kqueue>
{};
TEST_SUITE(
    native_tcp_socket_test_kqueue, "boost.corosio.native.tcp_socket.kqueue");
#endif

#if BOOST_COROSIO_HAS_IOCP
struct native_tcp_socket_test_iocp : native_tcp_socket_test<iocp>
{};
TEST_SUITE(native_tcp_socket_test_iocp, "boost.corosio.native.tcp_socket.iocp");
#endif

} // namespace boost::corosio
