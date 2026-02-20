//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/native/native_signal_set.hpp>
#include <boost/corosio/native/native_io_context.hpp>

#include <csignal>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_signal_set_test
{
    void testSignalSetConstruct()
    {
        io_context ctx(Backend);
        native_signal_set<Backend> ss(ctx);
        BOOST_TEST_PASS();
    }

    void testSignalSetConstructWithSignals()
    {
        io_context ctx(Backend);
        native_signal_set<Backend> ss(ctx, SIGINT);
        BOOST_TEST_PASS();
    }

    void testSignalSetPolymorphicSlice()
    {
        io_context ctx(Backend);
        native_signal_set<Backend> nss(ctx, SIGINT);

        signal_set& base = nss;
        (void)base;

        io_signal_set& io_base = nss;
        (void)io_base;

        BOOST_TEST_PASS();
    }

    void run()
    {
        testSignalSetConstruct();
        testSignalSetConstructWithSignals();
        testSignalSetPolymorphicSlice();
    }
};

#if BOOST_COROSIO_HAS_EPOLL
struct native_signal_set_test_epoll : native_signal_set_test<epoll>
{};
TEST_SUITE(
    native_signal_set_test_epoll, "boost.corosio.native.signal_set.epoll");
#endif

#if BOOST_COROSIO_HAS_SELECT
struct native_signal_set_test_select : native_signal_set_test<select>
{};
TEST_SUITE(
    native_signal_set_test_select, "boost.corosio.native.signal_set.select");
#endif

#if BOOST_COROSIO_HAS_KQUEUE
struct native_signal_set_test_kqueue : native_signal_set_test<kqueue>
{};
TEST_SUITE(
    native_signal_set_test_kqueue, "boost.corosio.native.signal_set.kqueue");
#endif

#if BOOST_COROSIO_HAS_IOCP
struct native_signal_set_test_iocp : native_signal_set_test<iocp>
{};
TEST_SUITE(native_signal_set_test_iocp, "boost.corosio.native.signal_set.iocp");
#endif

} // namespace boost::corosio
