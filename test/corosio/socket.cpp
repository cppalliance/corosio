//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/socket.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/async_run.hpp>

#include <iostream>

#undef NDEBUG
#include <cassert>

// Global counter for tracking I/O operations
std::size_t g_io_count = 0;

//------------------------------------------------
// Socket-specific tests
// Focus: socket awaitable interface, frame allocator, basic operation
//------------------------------------------------

// Simple coroutine that performs one async read operation
boost::capy::task<> read_once(boost::corosio::socket& sock)
{
    // Perform a single async read - this will suspend and resume via the reactor
    co_await sock.async_read_some();
    // When we get here, the read has "completed"
}

// Test that a simple coroutine calling async_read_some works correctly
void test_single_layer_coroutine()
{
    std::cout << "=== Test 1: Single-layer coroutine with mock socket ===\n";

    // Reset I/O counter
    g_io_count = 0;

    // Create I/O context (single-threaded)
    boost::corosio::io_context ioc;

    // Create a mock socket
    boost::corosio::socket sock(ioc);

    // Get executor
    auto ex = ioc.get_executor();

    // Launch the coroutine
    boost::capy::async_run(ex)(read_once(sock));

    // With inline dispatch, coroutine runs immediately until first I/O suspend
    // g_io_count is incremented when await_suspend is called
    assert(g_io_count == 1);
    std::cout << "After async_run, I/O count: " << g_io_count << " (expected 1)\n";

    // Run the I/O context to process the coroutine completion
    ioc.run();

    // The I/O operation was already started; ioc.run() resumes the coroutine
    assert(g_io_count == 1);
    std::cout << "After ioc.run(), I/O count: " << g_io_count << " (expected 1)\n";

    std::cout << "Test passed!\n";
}

// Test multiple sequential reads
boost::capy::task<> read_multiple(boost::corosio::socket& sock, int count)
{
    for(int i = 0; i < count; ++i)
    {
        co_await sock.async_read_some();
    }
}

void test_multiple_reads()
{
    std::cout << "\n=== Test 2: Multiple sequential reads ===\n";

    g_io_count = 0;

    boost::corosio::io_context ioc;
    boost::corosio::socket sock(ioc);
    auto ex = ioc.get_executor();

    const int read_count = 5;
    boost::capy::async_run(ex)(read_multiple(sock, read_count));

    // With inline dispatch, first I/O is started immediately
    assert(g_io_count == 1);
    std::cout << "After async_run, I/O count: " << g_io_count << " (expected 1)\n";

    ioc.run();

    assert(g_io_count == read_count);
    std::cout << "After ioc.run(), I/O count: " << g_io_count << " (expected " << read_count
              << ")\n";

    std::cout << "Test passed!\n";
}

// Test launching multiple coroutines
void test_multiple_coroutines()
{
    std::cout << "\n=== Test 4: Multiple concurrent coroutines ===\n";

    g_io_count = 0;

    boost::corosio::io_context ioc;
    boost::corosio::socket sock1(ioc);
    boost::corosio::socket sock2(ioc);
    boost::corosio::socket sock3(ioc);
    auto ex = ioc.get_executor();

    // Launch three coroutines
    boost::capy::async_run(ex)(read_once(sock1));
    boost::capy::async_run(ex)(read_once(sock2));
    boost::capy::async_run(ex)(read_once(sock3));

    // With inline dispatch, all 3 coroutines run to first I/O immediately
    assert(g_io_count == 3);
    std::cout << "After launching 3 coroutines, I/O count: " << g_io_count << " (expected 3)\n";

    ioc.run();

    // All three should have completed one read each
    assert(g_io_count == 3);
    std::cout << "After ioc.run(), I/O count: " << g_io_count << " (expected 3)\n";

    std::cout << "Test passed!\n";
}

// Test that await_ready returns false (always suspends)
void test_always_suspends()
{
    std::cout << "\n=== Test 5: async_read_some always suspends ===\n";

    boost::corosio::io_context ioc;
    boost::corosio::socket sock(ioc);

    auto awaitable = sock.async_read_some();
    (void)awaitable;

    // The awaitable should always suspend (return false from await_ready)
    assert(awaitable.await_ready() == false);
    std::cout << "await_ready() returns false (always suspends)\n";

    std::cout << "Test passed!\n";
}

//------------------------------------------------

int main()
{
    std::cout << "=== Socket Tests ===\n";
    std::cout << "Testing socket awaitable interface and basic operations\n\n";

    test_single_layer_coroutine();
    test_multiple_reads();
    test_multiple_coroutines();
    test_always_suspends();

    std::cout << "\n=== All tests passed! ===\n";
    return 0;
}
