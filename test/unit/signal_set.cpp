//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/signal_set.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

// Include platform-specific context headers for multi-backend testing
#if !defined(_WIN32)
#include <boost/corosio/select_context.hpp>
#endif

#include <csignal>
#include <chrono>

#include "test_suite.hpp"

namespace boost::corosio {

//------------------------------------------------
// Signal set tests
// Focus: construction, add/remove, wait, and cancellation
//
// Tests are templated on the context type to run with all available backends.
//------------------------------------------------

template<class Context>
struct signal_set_test_impl
{
    //--------------------------------------------
    // Construction and move semantics
    //--------------------------------------------

    void
    testConstruction()
    {
        Context ioc;
        signal_set s(ioc);

        BOOST_TEST_PASS();
    }

    void
    testConstructWithOneSignal()
    {
        Context ioc;
        signal_set s(ioc, SIGINT);

        BOOST_TEST_PASS();
    }

    void
    testConstructWithTwoSignals()
    {
        Context ioc;
        signal_set s(ioc, SIGINT, SIGTERM);

        BOOST_TEST_PASS();
    }

    void
    testConstructWithThreeSignals()
    {
        Context ioc;
        signal_set s(ioc, SIGINT, SIGTERM, SIGABRT);

        BOOST_TEST_PASS();
    }

    void
    testMoveConstruct()
    {
        Context ioc;
        signal_set s1(ioc, SIGINT);

        signal_set s2(std::move(s1));
        BOOST_TEST_PASS();
    }

    void
    testMoveAssign()
    {
        Context ioc;
        signal_set s1(ioc, SIGINT);
        signal_set s2(ioc);

        s2 = std::move(s1);
        BOOST_TEST_PASS();
    }

    void
    testMoveAssignCrossContextThrows()
    {
        Context ioc1;
        Context ioc2;
        signal_set s1(ioc1);
        signal_set s2(ioc2);

        BOOST_TEST_THROWS(s2 = std::move(s1), std::logic_error);
    }

    //--------------------------------------------
    // Add/remove/clear tests
    //--------------------------------------------

    void
    testAdd()
    {
        Context ioc;
        signal_set s(ioc);

        auto result = s.add(SIGINT);
        BOOST_TEST(result.has_value());
    }

    void
    testAddDuplicate()
    {
        Context ioc;
        signal_set s(ioc);

        BOOST_TEST(s.add(SIGINT).has_value());
        auto result = s.add(SIGINT);  // Should be no-op
        BOOST_TEST(result.has_value());
    }

    void
    testAddInvalidSignal()
    {
        Context ioc;
        signal_set s(ioc);

        auto result = s.add(-1);
        BOOST_TEST(result.has_error());
    }

    void
    testRemove()
    {
        Context ioc;
        signal_set s(ioc);

        BOOST_TEST(s.add(SIGINT).has_value());
        auto result = s.remove(SIGINT);
        BOOST_TEST(result.has_value());
    }

    void
    testRemoveNotPresent()
    {
        Context ioc;
        signal_set s(ioc);

        // Removing signal not in set should be a no-op
        auto result = s.remove(SIGINT);
        BOOST_TEST(result.has_value());
    }

    void
    testClear()
    {
        Context ioc;
        signal_set s(ioc);

        BOOST_TEST(s.add(SIGINT).has_value());
        BOOST_TEST(s.add(SIGTERM).has_value());
        BOOST_TEST(s.clear().has_value());
    }

    void
    testClearEmpty()
    {
        Context ioc;
        signal_set s(ioc);

        BOOST_TEST(s.clear().has_value());  // Should be no-op
    }

    //--------------------------------------------
    // Async wait tests
    //--------------------------------------------

    void
    testWaitWithSignal()
    {
        Context ioc;
        signal_set s(ioc, SIGINT);
        timer t(ioc);

        bool completed = false;
        int received_signal = 0;
        system::error_code result_ec;

        auto wait_task = [](signal_set& s_ref, system::error_code& ec_out, int& sig_out, bool& done_out) -> capy::task<>
        {
            auto [ec, signum] = co_await s_ref.async_wait();
            ec_out = ec;
            sig_out = signum;
            done_out = true;
        };
        capy::run_async(ioc.get_executor())(wait_task(s, result_ec, received_signal, completed));

        // Raise signal after a short delay
        t.expires_after(std::chrono::milliseconds(10));
        auto raise_task = [](timer& t_ref) -> capy::task<>
        {
            (void)co_await t_ref.wait();
            std::raise(SIGINT);
        };
        capy::run_async(ioc.get_executor())(raise_task(t));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
        BOOST_TEST_EQ(received_signal, SIGINT);
    }

    void
    testWaitWithDifferentSignal()
    {
        Context ioc;
        signal_set s(ioc, SIGTERM);
        timer t(ioc);

        bool completed = false;
        int received_signal = 0;

        auto wait_task = [](signal_set& s_ref, int& sig_out, bool& done_out) -> capy::task<>
        {
            auto [ec, signum] = co_await s_ref.async_wait();
            sig_out = signum;
            done_out = true;
            (void)ec;
        };
        capy::run_async(ioc.get_executor())(wait_task(s, received_signal, completed));

        t.expires_after(std::chrono::milliseconds(10));
        auto raise_task = [](timer& t_ref) -> capy::task<>
        {
            (void)co_await t_ref.wait();
            std::raise(SIGTERM);
        };
        capy::run_async(ioc.get_executor())(raise_task(t));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST_EQ(received_signal, SIGTERM);
    }

    //--------------------------------------------
    // Cancellation tests
    //--------------------------------------------

    void
    testCancel()
    {
        Context ioc;
        signal_set s(ioc, SIGINT);
        timer cancel_timer(ioc);

        bool completed = false;
        system::error_code result_ec;

        auto wait_task = [](signal_set& s_ref, system::error_code& ec_out, bool& done_out) -> capy::task<>
        {
            auto [ec, signum] = co_await s_ref.async_wait();
            ec_out = ec;
            done_out = true;
            (void)signum;
        };
        capy::run_async(ioc.get_executor())(wait_task(s, result_ec, completed));

        cancel_timer.expires_after(std::chrono::milliseconds(10));
        auto cancel_task = [](timer& t_ref, signal_set& s_ref) -> capy::task<>
        {
            (void)co_await t_ref.wait();
            s_ref.cancel();
        };
        capy::run_async(ioc.get_executor())(cancel_task(cancel_timer, s));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void
    testCancelNoWaiters()
    {
        Context ioc;
        signal_set s(ioc, SIGINT);

        s.cancel();  // Should be no-op
        BOOST_TEST_PASS();
    }

    void
    testCancelMultipleTimes()
    {
        Context ioc;
        signal_set s(ioc, SIGINT);

        s.cancel();
        s.cancel();
        s.cancel();
        BOOST_TEST_PASS();
    }

    //--------------------------------------------
    // Multiple signal set tests
    //--------------------------------------------

    void
    testMultipleSignalSetsOnSameSignal()
    {
        Context ioc;
        signal_set s1(ioc, SIGINT);
        signal_set s2(ioc, SIGINT);
        timer t(ioc);

        bool s1_completed = false;
        bool s2_completed = false;
        int s1_signal = 0;
        int s2_signal = 0;

        auto wait_task = [](signal_set& s_ref, int& sig_out, bool& done_out) -> capy::task<>
        {
            auto [ec, signum] = co_await s_ref.async_wait();
            sig_out = signum;
            done_out = true;
            (void)ec;
        };
        capy::run_async(ioc.get_executor())(wait_task(s1, s1_signal, s1_completed));
        capy::run_async(ioc.get_executor())(wait_task(s2, s2_signal, s2_completed));

        t.expires_after(std::chrono::milliseconds(10));
        auto raise_task = [](timer& t_ref) -> capy::task<>
        {
            (void)co_await t_ref.wait();
            std::raise(SIGINT);
        };
        capy::run_async(ioc.get_executor())(raise_task(t));

        ioc.run();
        BOOST_TEST(s1_completed);
        BOOST_TEST(s2_completed);
        BOOST_TEST_EQ(s1_signal, SIGINT);
        BOOST_TEST_EQ(s2_signal, SIGINT);
    }

    void
    testSignalSetWithMultipleSignals()
    {
        Context ioc;
        signal_set s(ioc, SIGINT, SIGTERM);
        timer t(ioc);

        bool completed = false;
        int received_signal = 0;

        auto wait_task = [](signal_set& s_ref, int& sig_out, bool& done_out) -> capy::task<>
        {
            auto [ec, signum] = co_await s_ref.async_wait();
            sig_out = signum;
            done_out = true;
            (void)ec;
        };
        capy::run_async(ioc.get_executor())(wait_task(s, received_signal, completed));

        // Raise SIGTERM (not SIGINT)
        t.expires_after(std::chrono::milliseconds(10));
        auto raise_task = [](timer& t_ref) -> capy::task<>
        {
            (void)co_await t_ref.wait();
            std::raise(SIGTERM);
        };
        capy::run_async(ioc.get_executor())(raise_task(t));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST_EQ(received_signal, SIGTERM);
    }

    //--------------------------------------------
    // Queued signal tests
    //--------------------------------------------

    void
    testQueuedSignal()
    {
        Context ioc;
        signal_set s(ioc, SIGINT);

        // Raise signal before starting wait
        std::raise(SIGINT);

        bool completed = false;
        int received_signal = 0;

        auto wait_task = [](signal_set& s_ref, int& sig_out, bool& done_out) -> capy::task<>
        {
            auto [ec, signum] = co_await s_ref.async_wait();
            sig_out = signum;
            done_out = true;
            (void)ec;
        };
        capy::run_async(ioc.get_executor())(wait_task(s, received_signal, completed));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST_EQ(received_signal, SIGINT);
    }

    //--------------------------------------------
    // Sequential wait tests
    //--------------------------------------------

    void
    testSequentialWaits()
    {
        Context ioc;
        signal_set s(ioc, SIGINT);
        timer t(ioc);

        int wait_count = 0;

        auto task = [](signal_set& s_ref, timer& t_ref, int& count_out) -> capy::task<>
        {
            // First wait
            t_ref.expires_after(std::chrono::milliseconds(5));
            (void)co_await t_ref.wait();
            std::raise(SIGINT);

            auto [ec1, sig1] = co_await s_ref.async_wait();
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(sig1, SIGINT);
            ++count_out;

            // Second wait
            t_ref.expires_after(std::chrono::milliseconds(5));
            (void)co_await t_ref.wait();
            std::raise(SIGINT);

            auto [ec2, sig2] = co_await s_ref.async_wait();
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(sig2, SIGINT);
            ++count_out;
        };
        capy::run_async(ioc.get_executor())(task(s, t, wait_count));

        ioc.run();
        BOOST_TEST_EQ(wait_count, 2);
    }

    //--------------------------------------------
    // io_result tests
    //--------------------------------------------

    void
    testIoResultSuccess()
    {
        Context ioc;
        signal_set s(ioc, SIGINT);
        timer t(ioc);

        bool result_ok = false;

        auto task = [](signal_set& s_ref, timer& t_ref, bool& ok_out) -> capy::task<>
        {
            t_ref.expires_after(std::chrono::milliseconds(5));
            (void)co_await t_ref.wait();
            std::raise(SIGINT);

            auto result = co_await s_ref.async_wait();
            ok_out = !result.ec;
        };
        capy::run_async(ioc.get_executor())(task(s, t, result_ok));

        ioc.run();
        BOOST_TEST(result_ok);
    }

    void
    testIoResultCanceled()
    {
        Context ioc;
        signal_set s(ioc, SIGINT);
        timer cancel_timer(ioc);

        bool result_ok = true;
        system::error_code result_ec;

        auto wait_task = [](signal_set& s_ref, bool& ok_out, system::error_code& ec_out) -> capy::task<>
        {
            auto result = co_await s_ref.async_wait();
            ok_out = !result.ec;
            ec_out = result.ec;
        };
        capy::run_async(ioc.get_executor())(wait_task(s, result_ok, result_ec));

        cancel_timer.expires_after(std::chrono::milliseconds(10));
        auto cancel_task = [](timer& t_ref, signal_set& s_ref) -> capy::task<>
        {
            (void)co_await t_ref.wait();
            s_ref.cancel();
        };
        capy::run_async(ioc.get_executor())(cancel_task(cancel_timer, s));

        ioc.run();
        BOOST_TEST(!result_ok);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void
    testIoResultStructuredBinding()
    {
        Context ioc;
        signal_set s(ioc, SIGINT);
        timer t(ioc);

        system::error_code captured_ec;
        int captured_signal = 0;

        auto task = [](signal_set& s_ref, timer& t_ref, system::error_code& ec_out, int& sig_out) -> capy::task<>
        {
            t_ref.expires_after(std::chrono::milliseconds(5));
            (void)co_await t_ref.wait();
            std::raise(SIGINT);

            auto [ec, signum] = co_await s_ref.async_wait();
            ec_out = ec;
            sig_out = signum;
        };
        capy::run_async(ioc.get_executor())(task(s, t, captured_ec, captured_signal));

        ioc.run();
        BOOST_TEST(!captured_ec);
        BOOST_TEST_EQ(captured_signal, SIGINT);
    }

    //--------------------------------------------
    // Signal flags tests (cross-platform)
    //--------------------------------------------

    void
    testFlagsBitwiseOperations()
    {
        // Test OR
        auto combined = signal_set::restart | signal_set::no_defer;
        BOOST_TEST((combined & signal_set::restart) != signal_set::none);
        BOOST_TEST((combined & signal_set::no_defer) != signal_set::none);
        BOOST_TEST((combined & signal_set::no_child_stop) == signal_set::none);

        // Test compound assignment
        auto flags = signal_set::none;
        flags |= signal_set::restart;
        BOOST_TEST((flags & signal_set::restart) != signal_set::none);

        // Test NOT
        auto all_but_restart = ~signal_set::restart;
        BOOST_TEST((all_but_restart & signal_set::restart) == signal_set::none);
    }

    void
    testAddWithNoneFlags()
    {
        Context ioc;
        signal_set s(ioc);

        // Add signal with none (default behavior) - works on all platforms
        auto result = s.add(SIGINT, signal_set::none);
        BOOST_TEST(result.has_value());
    }

    void
    testAddWithDontCareFlags()
    {
        Context ioc;
        signal_set s(ioc);

        // Add signal with dont_care - works on all platforms
        auto result = s.add(SIGINT, signal_set::dont_care);
        BOOST_TEST(result.has_value());
    }

#if !defined(_WIN32)
    //--------------------------------------------
    // Signal flags tests (POSIX only)
    // Windows returns operation_not_supported for
    // flags other than none/dont_care
    //--------------------------------------------

    void
    testAddWithFlags()
    {
        Context ioc;
        signal_set s(ioc);

        // Add signal with restart flag
        auto result = s.add(SIGINT, signal_set::restart);
        BOOST_TEST(result.has_value());
    }

    void
    testAddWithMultipleFlags()
    {
        Context ioc;
        signal_set s(ioc);

        // Add signal with combined flags
        auto result = s.add(SIGINT, signal_set::restart | signal_set::no_defer);
        BOOST_TEST(result.has_value());
    }

    void
    testAddSameSignalSameFlags()
    {
        Context ioc;
        signal_set s(ioc);

        // Add signal twice with same flags (should be no-op)
        BOOST_TEST(s.add(SIGINT, signal_set::restart).has_value());
        BOOST_TEST(s.add(SIGINT, signal_set::restart).has_value());
    }

    void
    testAddSameSignalDifferentFlags()
    {
        Context ioc;
        signal_set s(ioc);

        // Add signal with one flag, then try to add with different flag
        BOOST_TEST(s.add(SIGINT, signal_set::restart).has_value());
        auto result = s.add(SIGINT, signal_set::no_defer);
        BOOST_TEST(result.has_error());  // Should fail due to flag mismatch
    }

    void
    testAddSameSignalWithDontCare()
    {
        Context ioc;
        signal_set s(ioc);

        // Add signal with specific flags, then add with dont_care
        BOOST_TEST(s.add(SIGINT, signal_set::restart).has_value());
        auto result = s.add(SIGINT, signal_set::dont_care);
        BOOST_TEST(result.has_value());  // Should succeed with dont_care
    }

    void
    testAddSameSignalDontCareFirst()
    {
        Context ioc;
        signal_set s(ioc);

        // Add signal with dont_care, then add with specific flags
        BOOST_TEST(s.add(SIGINT, signal_set::dont_care).has_value());
        auto result = s.add(SIGINT, signal_set::restart);
        BOOST_TEST(result.has_value());  // Should succeed
    }

    void
    testMultipleSetsCompatibleFlags()
    {
        Context ioc;
        signal_set s1(ioc);
        signal_set s2(ioc);

        // Both sets add same signal with same flags
        BOOST_TEST(s1.add(SIGINT, signal_set::restart).has_value());
        BOOST_TEST(s2.add(SIGINT, signal_set::restart).has_value());
    }

    void
    testMultipleSetsIncompatibleFlags()
    {
        Context ioc;
        signal_set s1(ioc);
        signal_set s2(ioc);

        // First set adds with one flag
        BOOST_TEST(s1.add(SIGINT, signal_set::restart).has_value());
        // Second set tries to add with different flag
        auto result = s2.add(SIGINT, signal_set::no_defer);
        BOOST_TEST(result.has_error());  // Should fail
    }

    void
    testMultipleSetsWithDontCare()
    {
        Context ioc;
        signal_set s1(ioc);
        signal_set s2(ioc);

        // First set adds with specific flags
        BOOST_TEST(s1.add(SIGINT, signal_set::restart).has_value());
        // Second set adds with dont_care
        BOOST_TEST(s2.add(SIGINT, signal_set::dont_care).has_value());
    }

    void
    testWaitWithFlagsWorks()
    {
        Context ioc;
        signal_set s(ioc);
        timer t(ioc);

        // Add signal with restart flag and verify wait still works
        BOOST_TEST(s.add(SIGINT, signal_set::restart).has_value());

        bool completed = false;
        int received_signal = 0;

        auto wait_task = [](signal_set& s_ref, int& sig_out, bool& done_out) -> capy::task<>
        {
            auto [ec, signum] = co_await s_ref.async_wait();
            sig_out = signum;
            done_out = true;
            (void)ec;
        };
        capy::run_async(ioc.get_executor())(wait_task(s, received_signal, completed));

        t.expires_after(std::chrono::milliseconds(10));
        auto raise_task = [](timer& t_ref) -> capy::task<>
        {
            (void)co_await t_ref.wait();
            std::raise(SIGINT);
        };
        capy::run_async(ioc.get_executor())(raise_task(t));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST_EQ(received_signal, SIGINT);
    }

#else // _WIN32
    //--------------------------------------------
    // Signal flags tests (Windows only)
    //--------------------------------------------

    void
    testFlagsNotSupportedOnWindows()
    {
        Context ioc;
        signal_set s(ioc);

        // Windows returns operation_not_supported for actual flags
        auto result = s.add(SIGINT, signal_set::restart);
        BOOST_TEST(result.has_error());
        BOOST_TEST(result.error() == system::errc::operation_not_supported);
    }

#endif // _WIN32

    void
    run()
    {
        // Construction and move semantics
        testConstruction();
        testConstructWithOneSignal();
        testConstructWithTwoSignals();
        testConstructWithThreeSignals();
        testMoveConstruct();
        testMoveAssign();
        testMoveAssignCrossContextThrows();

        // Add/remove/clear tests
        testAdd();
        testAddDuplicate();
        testAddInvalidSignal();
        testRemove();
        testRemoveNotPresent();
        testClear();
        testClearEmpty();

        // Async wait tests
        testWaitWithSignal();
        testWaitWithDifferentSignal();

        // Cancellation tests
        testCancel();
        testCancelNoWaiters();
        testCancelMultipleTimes();

        // Multiple signal set tests
        testMultipleSignalSetsOnSameSignal();
        testSignalSetWithMultipleSignals();

        // Queued signal tests
        testQueuedSignal();

        // Sequential wait tests
        testSequentialWaits();

        // io_result tests
        testIoResultSuccess();
        testIoResultCanceled();
        testIoResultStructuredBinding();

        // Signal flags tests (cross-platform)
        testFlagsBitwiseOperations();
        testAddWithNoneFlags();
        testAddWithDontCareFlags();

#if !defined(_WIN32)
        // Signal flags tests (POSIX only)
        testAddWithFlags();
        testAddWithMultipleFlags();
        testAddSameSignalSameFlags();
        testAddSameSignalDifferentFlags();
        testAddSameSignalWithDontCare();
        testAddSameSignalDontCareFirst();
        testMultipleSetsCompatibleFlags();
        testMultipleSetsIncompatibleFlags();
        testMultipleSetsWithDontCare();
        testWaitWithFlagsWorks();
#else
        // Signal flags tests (Windows only)
        testFlagsNotSupportedOnWindows();
#endif
    }
};

//------------------------------------------------
// Register test suites for each available backend
//------------------------------------------------

// Default io_context (platform default backend)
struct signal_set_test : signal_set_test_impl<io_context> {};
TEST_SUITE(signal_set_test, "boost.corosio.signal_set");

// POSIX: also test with select_context explicitly
#if !defined(_WIN32)
struct signal_set_test_select : signal_set_test_impl<select_context> {};
TEST_SUITE(signal_set_test_select, "boost.corosio.signal_set.select");
#endif

} // namespace boost::corosio
