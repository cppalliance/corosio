//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4c.io-context.adoc.

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

// tag::assume[]
#include <boost/corosio/io_context.hpp>
#include <boost/capy/ex/run_async.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::assume[]

#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <coroutine>
#include <iostream>
#include <thread>
#include <vector>

#include "test_suite.hpp"

namespace {

bool my_coroutine_ran = false;

capy::task<> my_coroutine()
{
    my_coroutine_ran = true;
    co_return;
}

// Dispatching a continuation with no coroutine behind it would crash
// the loop; the fragment is compiled but never executed.
[[maybe_unused]] void
executor_ops_fragment(
    corosio::io_context& ioc,
    capy::continuation& cont,
    std::coroutine_handle<> handle)
{
    // tag::executor_ops[]
    auto ex = ioc.get_executor();

    // Dispatch a continuation: symmetric transfer if inside run(),
    // otherwise post. Returns a handle to resume.
    std::coroutine_handle<> next = ex.dispatch(cont);

    // Post a continuation: always queue for later execution
    ex.post(cont);

    // Post a bare coroutine handle for later execution
    ex.post(handle);
    // end::executor_ops[]
}

struct my_service : capy::execution_context::service
{
    explicit my_service(capy::execution_context&) {}

protected:
    void shutdown() override {}
};

struct io_context_test
{
    void
    testOverview()
    {
        // tag::overview[]
        corosio::io_context ioc;

        // ... create I/O objects and launch coroutines ...

        ioc.run();  // Process events until all work completes
        // end::overview[]
    }

    void
    testConstructDefault()
    {
        // tag::construct_default[]
        corosio::io_context ioc;
        // end::construct_default[]
    }

    void
    testConstructHintOne()
    {
        // tag::construct_hint[]
        corosio::io_context ioc(1);  // one thread expected to call run()
        // end::construct_hint[]
    }

    void
    testConstructHintFour()
    {
        // tag::construct_hint[]
        corosio::io_context ioc(4);  // up to 4 threads expected to call run()
        // end::construct_hint[]
    }

    void
    testRun()
    {
        corosio::io_context ioc;
        // tag::run[]
        std::size_t n = ioc.run();
        std::cout << "Processed " << n << " handlers\n";
        // end::run[]
        BOOST_TEST(n == 0);
    }

    void
    testRunOne()
    {
        corosio::io_context ioc;
        // tag::run_one[]
        std::size_t n = ioc.run_one();  // Returns 0 or 1
        // end::run_one[]
        BOOST_TEST(n == 0);
    }

    void
    testRunFor()
    {
        corosio::io_context ioc;
        // With no outstanding work both calls return immediately.
        auto deadline = std::chrono::steady_clock::now();
        // tag::run_for[]
        using namespace std::chrono_literals;

        auto n = ioc.run_for(100ms);   // Run for 100 milliseconds
        auto m = ioc.run_until(deadline);  // Run until time point
        // end::run_for[]
        BOOST_TEST(n == 0);
        BOOST_TEST(m == 0);
    }

    void
    testPoll()
    {
        corosio::io_context ioc;
        // tag::poll[]
        std::size_t n = ioc.poll();      // All ready handlers
        std::size_t m = ioc.poll_one();  // At most one ready handler
        // end::poll[]
        BOOST_TEST(n == 0);
        BOOST_TEST(m == 0);
    }

    void
    testStop()
    {
        corosio::io_context ioc;
        // tag::stop[]
        ioc.stop();
        // end::stop[]
        BOOST_TEST(ioc.stopped());
    }

    void
    testStopped()
    {
        corosio::io_context ioc;
        ioc.stop();
        // tag::stopped[]
        if (ioc.stopped())
            std::cout << "Event loop stopped\n";
        // end::stopped[]
        BOOST_TEST(ioc.stopped());
    }

    void
    testRestart()
    {
        corosio::io_context ioc;
        // tag::restart[]
        ioc.stop();
        // ... do something ...
        ioc.restart();
        ioc.run();  // Can run again
        // end::restart[]
        // run() stops the context again once the work runs out; the
        // restart() above is what allowed it to process at all.
        BOOST_TEST(ioc.stopped());
    }

    void
    testExecutor()
    {
        my_coroutine_ran = false;
        corosio::io_context ioc;
        // tag::executor[]
        auto ex = ioc.get_executor();

        // Launch a coroutine
        capy::run_async(ex)(my_coroutine());

        // Access the context
        corosio::io_context& ctx = ex.context();

        // Check if running inside the event loop
        if (ex.running_in_this_thread())
            std::cout << "Inside run()\n";
        // end::executor[]
        BOOST_TEST(&ctx == &ioc);
        ioc.run();
        BOOST_TEST(my_coroutine_ran);
    }

    void
    testWorkTracking()
    {
        corosio::io_context ioc;
        auto ex = ioc.get_executor();
        // tag::work_tracking[]
        ex.on_work_started();   // Increment work count
        ex.on_work_finished();  // Decrement work count
        // end::work_tracking[]
        BOOST_TEST(ioc.run() == 0);
    }

    void
    testMultithreaded()
    {
        // tag::multithreaded[]
        corosio::io_context ioc(4);

        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i)
            threads.emplace_back([&ioc] { ioc.run(); });

        for (auto& t : threads)
            t.join();
        // end::multithreaded[]
        BOOST_TEST(threads.size() == 4);
    }

    void
    testTeardown()
    {
        // tag::teardown[]
        corosio::io_context ioc(4);

        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i)
            threads.emplace_back([&ioc] { ioc.run(); });

        // Join every run() thread before ioc leaves scope.
        for (auto& t : threads)
            t.join();
        // end::teardown[]
        BOOST_TEST(threads.size() == 4);
    }

    void
    testServices()
    {
        corosio::io_context ioc;
        // tag::services[]
        // Create or get a service
        my_service& svc = ioc.use_service<my_service>();

        // Check if service exists
        if (ioc.has_service<my_service>())
        {
            // ...
        }
        // end::services[]
        BOOST_TEST(ioc.has_service<my_service>());
        BOOST_TEST(&ioc.use_service<my_service>() == &svc);
    }

    void
    run()
    {
        testOverview();
        testConstructDefault();
        testConstructHintOne();
        testConstructHintFour();
        testRun();
        testRunOne();
        testRunFor();
        testPoll();
        testStop();
        testStopped();
        testRestart();
        testExecutor();
        testWorkTracking();
        testMultithreaded();
        testTeardown();
        testServices();
    }
};

} // namespace

TEST_SUITE(io_context_test, "boost.corosio.doc.4c_io_context");
