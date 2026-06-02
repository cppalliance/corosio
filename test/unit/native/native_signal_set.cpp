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
#include <type_traits>
#include <utility>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_signal_set_test
{
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_signal_set<Backend>&>().wait()),
            decltype(std::declval<io_signal_set&>().wait())>,
        "native_signal_set::wait must shadow io_signal_set::wait");

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

COROSIO_BACKEND_TESTS(native_signal_set_test, "boost.corosio.native.signal_set")

} // namespace boost::corosio
