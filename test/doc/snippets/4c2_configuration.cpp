//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4c2.configuration.adoc.

// Fragments deliberately leave results and bindings unused; the pages
// explain the values in prose instead.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-lambda-capture"
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4834) // discarding [[nodiscard]] return value
#pragma warning(disable: 4189) // local variable initialized but not referenced
#pragma warning(disable: 4100) // unreferenced formal parameter
#pragma warning(disable: 4101) // unreferenced local variable
#pragma warning(disable: 4456) // declaration hides previous local declaration
#pragma warning(disable: 4457) // declaration hides function parameter
#pragma warning(disable: 4458) // declaration hides class member
#pragma warning(disable: 4459) // declaration hides global declaration
#endif

// tag::options_basic_include[]
#include <boost/corosio/io_context.hpp>
// end::options_basic_include[]
// tag::options_native_include[]
#include <boost/corosio/native/native_io_context.hpp>
// end::options_native_include[]

#include "test_suite.hpp"

namespace corosio = boost::corosio;

namespace {

struct configuration_test
{
    void
    testOptionsBasic()
    {
        // tag::options_basic[]
        corosio::io_context_options opts;
        opts.max_events_per_poll = 256;
        opts.inline_budget_max   = 32;

        corosio::io_context ioc(opts);
        // end::options_basic[]
        ioc.run();
        BOOST_TEST(opts.max_events_per_poll == 256u);
    }

    void
    testOptionsNative()
    {
#if BOOST_COROSIO_HAS_EPOLL
        // tag::options_native[]
        corosio::io_context_options opts;
        opts.max_events_per_poll = 512;

        corosio::native_io_context<corosio::epoll> ioc(opts);
        // end::options_native[]
        ioc.run();
        BOOST_TEST(opts.max_events_per_poll == 512u);
#endif
    }

    void
    testLockingUnsafe()
    {
        // tag::locking_unsafe[]
        corosio::io_context_options opts;
        opts.locking = corosio::locking_mode::unsafe;

        corosio::io_context ioc(opts);
        ioc.run();  // only one thread may call this
        // end::locking_unsafe[]
        BOOST_TEST(opts.locking == corosio::locking_mode::unsafe);
    }

    void
    run()
    {
        testOptionsBasic();
        testOptionsNative();
        testLockingUnsafe();
    }
};

} // namespace

TEST_SUITE(configuration_test, "boost.corosio.doc.4c2_configuration");
