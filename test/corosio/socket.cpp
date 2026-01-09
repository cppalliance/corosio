//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <corosio/io_context.hpp>
#include <corosio/socket.hpp>
#include <capy/task.hpp>
#include <capy/async_run.hpp>

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
capy::task read_once(corosio::socket& sock)
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
    corosio::io_context ioc;

    // Create a mock socket
    corosio::socket sock(ioc);

    // Get executor
    auto ex = ioc.get_executor();

    // Launch the coroutine
    capy::async_run(ex, read_once(sock));

    // At this point, the coroutine has been posted but not executed
    assert(g_io_count == 0);
    std::cout << "After async_run, I/O count: " << g_io_count << " (expected 0)\n";

    // Run the I/O context to process the coroutine
    ioc.run();

    // The coroutine should have:
    // 1. Started and hit the co_await
    // 2. Called socket.async_read_some() which increments g_io_count
    // 3. Suspended and posted a read_state work item
    // 4. The reactor processed the work item
    // 5. The coroutine resumed and completed
    assert(g_io_count == 1);
    std::cout << "After ioc.run(), I/O count: " << g_io_count << " (expected 1)\n";

    std::cout << "Test passed!\n";
}

// Test multiple sequential reads
capy::task read_multiple(corosio::socket& sock, int count)
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

    corosio::io_context ioc;
    corosio::socket sock(ioc);
    auto ex = ioc.get_executor();

    const int read_count = 5;
    capy::async_run(ex, read_multiple(sock, read_count));

    assert(g_io_count == 0);
    std::cout << "After async_run, I/O count: " << g_io_count << " (expected 0)\n";

    ioc.run();

    assert(g_io_count == read_count);
    std::cout << "After ioc.run(), I/O count: " << g_io_count << " (expected " << read_count
              << ")\n";

    std::cout << "Test passed!\n";
}

// Test that the socket's frame allocator is accessible
void test_frame_allocator_access()
{
    std::cout << "\n=== Test 3: Frame allocator accessibility ===\n";

    corosio::io_context ioc;
    corosio::socket sock(ioc);

    // Access the frame allocator (should not crash)
    auto& allocator = sock.get_frame_allocator();
    (void)allocator; // Suppress unused variable warning

    std::cout << "Frame allocator accessible\n";
    std::cout << "Test passed!\n";
}

// Test launching multiple coroutines
void test_multiple_coroutines()
{
    std::cout << "\n=== Test 4: Multiple concurrent coroutines ===\n";

    g_io_count = 0;

    corosio::io_context ioc;
    corosio::socket sock1(ioc);
    corosio::socket sock2(ioc);
    corosio::socket sock3(ioc);
    auto ex = ioc.get_executor();

    // Launch three coroutines
    capy::async_run(ex, read_once(sock1));
    capy::async_run(ex, read_once(sock2));
    capy::async_run(ex, read_once(sock3));

    assert(g_io_count == 0);
    std::cout << "After launching 3 coroutines, I/O count: " << g_io_count << " (expected 0)\n";

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

    corosio::io_context ioc;
    corosio::socket sock(ioc);

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
    test_frame_allocator_access();
    test_multiple_coroutines();
    test_always_suspends();

    std::cout << "\n=== All tests passed! ===\n";
    return 0;
}
