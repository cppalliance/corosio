//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/native/native_tcp_acceptor.hpp>
#include <boost/corosio/native/native_io_context.hpp>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_tcp_acceptor_test
{
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
        auto ec = a1.listen(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        BOOST_TEST(a1.is_open());

        native_tcp_acceptor<Backend> a2(std::move(a1));
        BOOST_TEST(a2.is_open());
    }

    void testAcceptorPolymorphicSlice()
    {
        io_context ctx(Backend);
        native_tcp_acceptor<Backend> na(ctx);
        auto ec = na.listen(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        tcp_acceptor& base = na;
        BOOST_TEST(base.is_open());
    }

    void run()
    {
        testAcceptorConstruct();
        testAcceptorMoveConstruct();
        testAcceptorPolymorphicSlice();
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
