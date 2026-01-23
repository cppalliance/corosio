//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/resolver.hpp>

// GCC emits false-positive "may be used uninitialized" warnings
// for structured bindings with co_await expressions
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/url/ipv4_address.hpp>
#include <boost/url/ipv6_address.hpp>

#include "test_suite.hpp"

namespace boost {
namespace corosio {

struct resolver_test
{
    //--------------------------------------------
    // Construction and move semantics
    //--------------------------------------------

    void
    testConstruction()
    {
        io_context ioc;
        resolver r(ioc);

        BOOST_TEST_PASS();
    }

    void
    testConstructionFromExecutor()
    {
        io_context ioc;
        resolver r(ioc.get_executor());

        BOOST_TEST_PASS();
    }

    void
    testMoveConstruct()
    {
        io_context ioc;
        resolver r1(ioc);
        resolver r2(std::move(r1));

        BOOST_TEST_PASS();
    }

    void
    testMoveAssign()
    {
        io_context ioc;
        resolver r1(ioc);
        resolver r2(ioc);

        r2 = std::move(r1);

        BOOST_TEST_PASS();
    }

    void
    testMoveAssignCrossContextThrows()
    {
        io_context ioc1;
        io_context ioc2;
        resolver r1(ioc1);
        resolver r2(ioc2);

        BOOST_TEST_THROWS(r2 = std::move(r1), std::logic_error);
    }

    //--------------------------------------------
    // Basic resolution tests
    //--------------------------------------------

    void
    testResolveLocalhost()
    {
        io_context ioc;
        resolver r(ioc);

        bool completed = false;
        system::error_code result_ec;
        resolver_results results;

        auto task = [](resolver& r_ref,
                       system::error_code& ec_out,
                       resolver_results& results_out,
                       bool& done_out) -> capy::task<>
        {
            auto [ec, res] = co_await r_ref.resolve("localhost", "80");
            ec_out = ec;
            results_out = std::move(res);
            done_out = true;
        };
        capy::run_async(ioc.get_executor())(
            task(r, result_ec, results, completed));

        ioc.run();

        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
        BOOST_TEST(!results.empty());

        // localhost should resolve to at least one address
        BOOST_TEST(results.size() >= 1);

        // Check that we got a valid endpoint
        bool found_valid = false;
        for (auto const& entry : results)
        {
            auto ep = entry.get_endpoint();
            // Port should be 80
            BOOST_TEST_EQ(ep.port(), 80);

            // Should be either 127.0.0.1 (IPv4) or ::1 (IPv6)
            if (ep.is_v4())
            {
                auto addr = ep.v4_address();
                if (addr == urls::ipv4_address({127, 0, 0, 1}))
                    found_valid = true;
            }
            else if (ep.is_v6())
            {
                auto addr = ep.v6_address();
                if (addr == urls::ipv6_address::loopback())
                    found_valid = true;
            }
        }
        BOOST_TEST(found_valid);
    }

    void
    testResolveNumericIPv4()
    {
        io_context ioc;
        resolver r(ioc);

        bool completed = false;
        system::error_code result_ec;
        resolver_results results;

        auto task = [](resolver& r_ref,
                       system::error_code& ec_out,
                       resolver_results& results_out,
                       bool& done_out) -> capy::task<>
        {
            auto [ec, res] = co_await r_ref.resolve(
                "127.0.0.1", "8080",
                resolve_flags::numeric_host | resolve_flags::numeric_service);
            ec_out = ec;
            results_out = std::move(res);
            done_out = true;
        };
        capy::run_async(ioc.get_executor())(
            task(r, result_ec, results, completed));

        ioc.run();

        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
        BOOST_TEST(!results.empty());
        BOOST_TEST_EQ(results.size(), 1u);

        auto const& entry = *results.begin();
        auto ep = entry.get_endpoint();
        BOOST_TEST(ep.is_v4());
        BOOST_TEST_EQ(ep.port(), 8080);
        BOOST_TEST(ep.v4_address() == urls::ipv4_address({127, 0, 0, 1}));
    }

    void
    testResolveNumericIPv6()
    {
        io_context ioc;
        resolver r(ioc);

        bool completed = false;
        system::error_code result_ec;
        resolver_results results;

        auto task = [](resolver& r_ref,
                       system::error_code& ec_out,
                       resolver_results& results_out,
                       bool& done_out) -> capy::task<>
        {
            auto [ec, res] = co_await r_ref.resolve(
                "::1", "443",
                resolve_flags::numeric_host | resolve_flags::numeric_service);
            ec_out = ec;
            results_out = std::move(res);
            done_out = true;
        };
        capy::run_async(ioc.get_executor())(
            task(r, result_ec, results, completed));

        ioc.run();

        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
        BOOST_TEST(!results.empty());
        BOOST_TEST_EQ(results.size(), 1u);

        auto const& entry = *results.begin();
        auto ep = entry.get_endpoint();
        BOOST_TEST(ep.is_v6());
        BOOST_TEST_EQ(ep.port(), 443);
        BOOST_TEST(ep.v6_address() == urls::ipv6_address::loopback());
    }

    void
    testResolveServiceName()
    {
        io_context ioc;
        resolver r(ioc);

        bool completed = false;
        system::error_code result_ec;
        resolver_results results;

        auto task = [](resolver& r_ref,
                       system::error_code& ec_out,
                       resolver_results& results_out,
                       bool& done_out) -> capy::task<>
        {
            auto [ec, res] = co_await r_ref.resolve(
                "127.0.0.1", "http",
                resolve_flags::numeric_host);
            ec_out = ec;
            results_out = std::move(res);
            done_out = true;
        };
        capy::run_async(ioc.get_executor())(
            task(r, result_ec, results, completed));

        ioc.run();

        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
        BOOST_TEST(!results.empty());

        // "http" should resolve to port 80
        auto const& entry = *results.begin();
        auto ep = entry.get_endpoint();
        BOOST_TEST_EQ(ep.port(), 80);
    }

    //--------------------------------------------
    // Entry metadata tests
    //--------------------------------------------

    void
    testEntryHostName()
    {
        io_context ioc;
        resolver r(ioc);

        bool completed = false;
        resolver_results results;

        auto task = [](resolver& r_ref,
                       resolver_results& results_out,
                       bool& done_out) -> capy::task<>
        {
            auto [ec, res] = co_await r_ref.resolve("localhost", "80");
            results_out = std::move(res);
            done_out = true;
        };
        capy::run_async(ioc.get_executor())(task(r, results, completed));

        ioc.run();

        BOOST_TEST(completed);
        BOOST_TEST(!results.empty());

        auto const& entry = *results.begin();
        BOOST_TEST_EQ(entry.host_name(), "localhost");
        BOOST_TEST_EQ(entry.service_name(), "80");
    }

    //--------------------------------------------
    // Error handling tests
    //--------------------------------------------

    void
    testResolveInvalidHost()
    {
        io_context ioc;
        resolver r(ioc);

        bool completed = false;
        system::error_code result_ec;
        resolver_results results;

        auto task = [](resolver& r_ref,
                       system::error_code& ec_out,
                       resolver_results& results_out,
                       bool& done_out) -> capy::task<>
        {
            // Use a definitely invalid hostname
            auto [ec, res] = co_await r_ref.resolve(
                "this.hostname.definitely.does.not.exist.invalid", "80");
            ec_out = ec;
            results_out = std::move(res);
            done_out = true;
        };
        capy::run_async(ioc.get_executor())(
            task(r, result_ec, results, completed));

        ioc.run();

        BOOST_TEST(completed);
        BOOST_TEST(result_ec);  // Should have an error
        BOOST_TEST(results.empty());
    }

    void
    testResolveInvalidNumericHost()
    {
        io_context ioc;
        resolver r(ioc);

        bool completed = false;
        system::error_code result_ec;
        resolver_results results;

        auto task = [](resolver& r_ref,
                       system::error_code& ec_out,
                       resolver_results& results_out,
                       bool& done_out) -> capy::task<>
        {
            // numeric_host flag with non-numeric hostname should fail
            auto [ec, res] = co_await r_ref.resolve(
                "localhost", "80",
                resolve_flags::numeric_host);
            ec_out = ec;
            results_out = std::move(res);
            done_out = true;
        };
        capy::run_async(ioc.get_executor())(
            task(r, result_ec, results, completed));

        ioc.run();

        BOOST_TEST(completed);
        BOOST_TEST(result_ec);  // Should have an error
    }

    //--------------------------------------------
    // Cancellation tests
    //--------------------------------------------

    void
    testCancel()
    {
        io_context ioc;
        resolver r(ioc);
        timer cancel_timer(ioc);

        bool completed = false;
        system::error_code result_ec;

        // Use a hostname that might take time to resolve (or timeout)
        // But cancel immediately
        auto wait_task = [](resolver& r_ref,
                            system::error_code& ec_out,
                            bool& done_out) -> capy::task<>
        {
            auto [ec, res] = co_await r_ref.resolve("localhost", "80");
            ec_out = ec;
            done_out = true;
        };
        capy::run_async(ioc.get_executor())(
            wait_task(r, result_ec, completed));

        // Cancel immediately
        r.cancel();

        ioc.run();

        BOOST_TEST(completed);
        // May or may not be canceled depending on timing
        // If it completes before cancel, that's fine too
    }

    void
    testCancelNoOperation()
    {
        io_context ioc;
        resolver r(ioc);

        // Cancel with no pending operation should not crash
        r.cancel();
        r.cancel();

        BOOST_TEST_PASS();
    }

    //--------------------------------------------
    // Sequential resolution tests
    //--------------------------------------------

    void
    testSequentialResolves()
    {
        io_context ioc;
        resolver r(ioc);

        int resolve_count = 0;

        auto task = [](resolver& r_ref, int& count_out) -> capy::task<>
        {
            // First resolve
            auto [ec1, res1] = co_await r_ref.resolve(
                "127.0.0.1", "80",
                resolve_flags::numeric_host | resolve_flags::numeric_service);
            BOOST_TEST(!ec1);
            BOOST_TEST(!res1.empty());
            ++count_out;

            // Second resolve
            auto [ec2, res2] = co_await r_ref.resolve(
                "127.0.0.1", "443",
                resolve_flags::numeric_host | resolve_flags::numeric_service);
            BOOST_TEST(!ec2);
            BOOST_TEST(!res2.empty());
            ++count_out;

            // Third resolve
            auto [ec3, res3] = co_await r_ref.resolve(
                "::1", "8080",
                resolve_flags::numeric_host | resolve_flags::numeric_service);
            BOOST_TEST(!ec3);
            BOOST_TEST(!res3.empty());
            ++count_out;
        };
        capy::run_async(ioc.get_executor())(task(r, resolve_count));

        ioc.run();

        BOOST_TEST_EQ(resolve_count, 3);
    }

    //--------------------------------------------
    // io_result tests
    //--------------------------------------------

    void
    testIoResultSuccess()
    {
        io_context ioc;
        resolver r(ioc);

        bool result_ok = false;

        auto task = [](resolver& r_ref, bool& ok_out) -> capy::task<>
        {
            auto result = co_await r_ref.resolve(
                "127.0.0.1", "80",
                resolve_flags::numeric_host | resolve_flags::numeric_service);
            ok_out = !result.ec;
        };
        capy::run_async(ioc.get_executor())(task(r, result_ok));

        ioc.run();

        BOOST_TEST(result_ok);
    }

    void
    testIoResultError()
    {
        io_context ioc;
        resolver r(ioc);

        bool got_error = false;
        system::error_code result_ec;

        auto task = [](resolver& r_ref, bool& error_out,
                       system::error_code& ec_out) -> capy::task<>
        {
            auto result = co_await r_ref.resolve(
                "not-a-valid-ip", "80",
                resolve_flags::numeric_host);
            error_out = static_cast<bool>(result.ec);
            ec_out = result.ec;
        };
        capy::run_async(ioc.get_executor())(task(r, got_error, result_ec));

        ioc.run();

        BOOST_TEST(got_error);
        BOOST_TEST(result_ec);
    }

    void
    testIoResultStructuredBinding()
    {
        io_context ioc;
        resolver r(ioc);

        system::error_code captured_ec;
        std::size_t result_size = 0;

        auto task = [](resolver& r_ref,
                       system::error_code& ec_out,
                       std::size_t& size_out) -> capy::task<>
        {
            auto [ec, results] = co_await r_ref.resolve(
                "127.0.0.1", "80",
                resolve_flags::numeric_host | resolve_flags::numeric_service);
            ec_out = ec;
            size_out = results.size();
        };
        capy::run_async(ioc.get_executor())(task(r, captured_ec, result_size));

        ioc.run();

        BOOST_TEST(!captured_ec);
        BOOST_TEST_EQ(result_size, 1u);
    }

    //--------------------------------------------
    // resolve_flags tests
    //--------------------------------------------

    void
    testResolveFlagsOperators()
    {
        // Test bitwise OR
        auto flags = resolve_flags::passive | resolve_flags::numeric_host;
        BOOST_TEST((flags & resolve_flags::passive) != resolve_flags::none);
        BOOST_TEST((flags & resolve_flags::numeric_host) != resolve_flags::none);
        BOOST_TEST((flags & resolve_flags::numeric_service) == resolve_flags::none);

        // Test bitwise OR assignment
        flags |= resolve_flags::numeric_service;
        BOOST_TEST((flags & resolve_flags::numeric_service) != resolve_flags::none);

        // Test bitwise AND assignment
        flags &= resolve_flags::numeric_host;
        BOOST_TEST((flags & resolve_flags::numeric_host) != resolve_flags::none);
        BOOST_TEST((flags & resolve_flags::passive) == resolve_flags::none);
    }

    //--------------------------------------------
    // resolver_results tests
    //--------------------------------------------

    void
    testResolverResultsEmpty()
    {
        resolver_results empty;
        BOOST_TEST(empty.empty());
        BOOST_TEST_EQ(empty.size(), 0u);
        BOOST_TEST(empty.begin() == empty.end());
    }

    void
    testResolverResultsIteration()
    {
        io_context ioc;
        resolver r(ioc);

        resolver_results results;

        auto task = [](resolver& r_ref,
                       resolver_results& results_out) -> capy::task<>
        {
            auto [ec, res] = co_await r_ref.resolve("localhost", "80");
            results_out = std::move(res);
        };
        capy::run_async(ioc.get_executor())(task(r, results));

        ioc.run();

        // Test range-based for
        std::size_t count = 0;
        for (auto const& entry : results)
        {
            (void)entry;
            ++count;
        }
        BOOST_TEST_EQ(count, results.size());

        // Test cbegin/cend
        count = 0;
        for (auto it = results.cbegin(); it != results.cend(); ++it)
            ++count;
        BOOST_TEST_EQ(count, results.size());
    }

    void
    testResolverResultsSwap()
    {
        std::vector<resolver_entry> entries1;
        entries1.emplace_back(
            endpoint(urls::ipv4_address({127, 0, 0, 1}), 80),
            "host1", "80");

        std::vector<resolver_entry> entries2;
        entries2.emplace_back(
            endpoint(urls::ipv4_address({192, 168, 1, 1}), 443),
            "host2", "443");
        entries2.emplace_back(
            endpoint(urls::ipv4_address({192, 168, 1, 2}), 443),
            "host2", "443");

        resolver_results r1(std::move(entries1));
        resolver_results r2(std::move(entries2));

        BOOST_TEST_EQ(r1.size(), 1u);
        BOOST_TEST_EQ(r2.size(), 2u);

        r1.swap(r2);

        BOOST_TEST_EQ(r1.size(), 2u);
        BOOST_TEST_EQ(r2.size(), 1u);
    }

    //--------------------------------------------
    // resolver_entry tests
    //--------------------------------------------

    void
    testResolverEntryConstruction()
    {
        endpoint ep(urls::ipv4_address({127, 0, 0, 1}), 8080);
        resolver_entry entry(ep, "myhost", "myservice");

        BOOST_TEST(entry.get_endpoint() == ep);
        BOOST_TEST_EQ(entry.host_name(), "myhost");
        BOOST_TEST_EQ(entry.service_name(), "myservice");
    }

    void
    testResolverEntryImplicitConversion()
    {
        endpoint ep(urls::ipv4_address({10, 0, 0, 1}), 9000);
        resolver_entry entry(ep, "test", "9000");

        // Test implicit conversion to endpoint
        endpoint converted = entry;
        BOOST_TEST(converted == ep);
    }

    void
    run()
    {
        // Construction and move semantics
        testConstruction();
        testConstructionFromExecutor();
        testMoveConstruct();
        testMoveAssign();
        testMoveAssignCrossContextThrows();

        // Basic resolution
        testResolveLocalhost();
        testResolveNumericIPv4();
        testResolveNumericIPv6();
        testResolveServiceName();

        // Entry metadata
        testEntryHostName();

        // Error handling
        testResolveInvalidHost();
        testResolveInvalidNumericHost();

        // Cancellation
        testCancel();
        testCancelNoOperation();

        // Sequential resolves
        testSequentialResolves();

        // io_result
        testIoResultSuccess();
        testIoResultError();
        testIoResultStructuredBinding();

        // resolve_flags
        testResolveFlagsOperators();

        // resolver_results
        testResolverResultsEmpty();
        testResolverResultsIteration();
        testResolverResultsSwap();

        // resolver_entry
        testResolverEntryConstruction();
        testResolverEntryImplicitConversion();
    }
};

TEST_SUITE(resolver_test, "boost.corosio.resolver");

} // namespace corosio
} // namespace boost
