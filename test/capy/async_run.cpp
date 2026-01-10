//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/capy/async_run.hpp>
#include <boost/capy/executor.hpp>
#include <boost/capy/executor_work.hpp>
#include <boost/capy/task.hpp>

#include <coroutine>
#include <iostream>
#include <queue>

#undef NDEBUG
#include <cassert>

// Global counter for tracking async operations
std::size_t g_suspend_count = 0;
std::size_t g_resume_count = 0;

// Global pointer to current test context's work queue
std::queue<boost::capy::executor_work*>* g_work_queue = nullptr;

//------------------------------------------------
// Mock executor and awaitable for testing async_run
// (no dependency on corosio)
//------------------------------------------------

/** Simple executor for testing.

    Maintains a queue of work items and processes them when run() is called.
*/
struct mock_executor : boost::capy::executor_base
{
    std::queue<boost::capy::executor_work*>* queue_;

    mock_executor() : queue_(nullptr) {}
    mock_executor(std::queue<boost::capy::executor_work*>* q) : queue_(q) {}

    boost::capy::coro dispatch(boost::capy::coro h) const override { return h; }

    void post(boost::capy::executor_work* w) const override
    {
        if(queue_)
            queue_->push(w);
    }

    bool operator==(mock_executor const& other) const noexcept { return queue_ == other.queue_; }
};

/** Mock context that processes queued work items. */
struct mock_context
{
    std::queue<boost::capy::executor_work*> work_queue_;

    mock_executor get_executor() { return {&work_queue_}; }

    void run()
    {
        while(!work_queue_.empty())
        {
            auto* work = work_queue_.front();
            work_queue_.pop();
            (*work)();
        }
    }
};

/** Mock async operation that suspends and resumes.

    This mimics the behavior of boost::corosio::socket::async_read_some()
    but without any dependency on corosio. Uses the global g_work_queue
    for posting work.
*/
struct mock_async_op
{
    bool await_ready() const noexcept { return false; }

    template<boost::capy::dispatcher D>
    std::coroutine_handle<> await_suspend(boost::capy::coro h, D const& d)
    {
        ++g_suspend_count;
        // Post the continuation back to the global work queue
        // (mimics what socket does when posting to reactor)
        g_work_queue->push(new resume_work<D>{h, d});
        // Return noop because we post work rather than resuming inline
        return std::noop_coroutine();
    }

    void await_resume() noexcept { ++g_resume_count; }

private:
    template<boost::capy::dispatcher D>
    struct resume_work final : boost::capy::executor_work
    {
        boost::capy::coro h_;
        D d_;

        resume_work(boost::capy::coro h, D const& d)
            : h_(h)
            , d_(d)
        {
        }

        ~resume_work() = default;

        void operator()() override
        {
            auto h = h_;
            auto d = d_;
            delete this;
            d(h)();
        }

        void destroy() override { delete this; }
    };
};

//------------------------------------------------
// Test: Single-layer coroutine with mock operation
//------------------------------------------------

boost::capy::task<> async_op_once()
{
    co_await mock_async_op{};
}

void test_single_layer_coroutine()
{
    std::cout << "=== Test 1: Single-layer coroutine with mock async_op ===\n";

    g_suspend_count = 0;
    g_resume_count = 0;

    mock_context ctx;
    g_work_queue = &ctx.work_queue_;
    auto ex = ctx.get_executor();

    boost::capy::async_run(ex)(async_op_once());

    // With inline dispatch, the coroutine runs immediately until it suspends
    assert(g_suspend_count == 1);
    assert(g_resume_count == 0);
    std::cout << "After async_run, suspend: " << g_suspend_count << ", resume: " << g_resume_count
              << " (expected 1, 0)\n";

    ctx.run();

    assert(g_suspend_count == 1);
    assert(g_resume_count == 1);
    std::cout << "After ctx.run(), suspend: " << g_suspend_count << ", resume: " << g_resume_count
              << " (expected 1, 1)\n";

    std::cout << "Test passed!\n";
}

//------------------------------------------------
// Test: Multiple sequential operations
//------------------------------------------------

boost::capy::task<> async_op_multiple(int count)
{
    for(int i = 0; i < count; ++i)
    {
        co_await mock_async_op{};
    }
}

void test_multiple_operations()
{
    std::cout << "\n=== Test 2: Multiple sequential operations ===\n";

    g_suspend_count = 0;
    g_resume_count = 0;

    mock_context ctx;
    g_work_queue = &ctx.work_queue_;
    auto ex = ctx.get_executor();

    const int op_count = 5;
    boost::capy::async_run(ex)(async_op_multiple(op_count));

    // With inline dispatch, first suspend happens immediately
    assert(g_suspend_count == 1);
    assert(g_resume_count == 0);
    std::cout << "After async_run, suspend: " << g_suspend_count << ", resume: " << g_resume_count
              << " (expected 1, 0)\n";

    ctx.run();

    assert(g_suspend_count == op_count);
    assert(g_resume_count == op_count);
    std::cout << "After ctx.run(), suspend: " << g_suspend_count << ", resume: " << g_resume_count
              << " (expected " << op_count << ", " << op_count << ")\n";

    std::cout << "Test passed!\n";
}

//------------------------------------------------
// Test: Multiple concurrent coroutines
//------------------------------------------------

void test_multiple_coroutines()
{
    std::cout << "\n=== Test 3: Multiple concurrent coroutines ===\n";

    g_suspend_count = 0;
    g_resume_count = 0;

    mock_context ctx;
    g_work_queue = &ctx.work_queue_;
    auto ex = ctx.get_executor();

    boost::capy::async_run(ex)(async_op_once());
    boost::capy::async_run(ex)(async_op_once());
    boost::capy::async_run(ex)(async_op_once());

    // With inline dispatch, all 3 coroutines run to first suspend immediately
    assert(g_suspend_count == 3);
    assert(g_resume_count == 0);
    std::cout << "After launching 3 coroutines, suspend: " << g_suspend_count
              << ", resume: " << g_resume_count << " (expected 3, 0)\n";

    ctx.run();

    assert(g_suspend_count == 3);
    assert(g_resume_count == 3);
    std::cout << "After ctx.run(), suspend: " << g_suspend_count << ", resume: " << g_resume_count
              << " (expected 3, 3)\n";

    std::cout << "Test passed!\n";
}

//------------------------------------------------
// Test: 3-level nested coroutines
//------------------------------------------------

boost::capy::task<> level3_op()
{
    std::cout << "  Level 3: Before async operation\n";
    co_await mock_async_op{};
    std::cout << "  Level 3: After async operation\n";
}

boost::capy::task<> level2_op()
{
    std::cout << " Level 2: Before calling level 3\n";
    co_await level3_op();
    std::cout << " Level 2: After level 3 returned\n";

    std::cout << " Level 2: Before own async operation\n";
    co_await mock_async_op{};
    std::cout << " Level 2: After own async operation\n";
}

boost::capy::task<> level1_op()
{
    std::cout << "Level 1: Before calling level 2\n";
    co_await level2_op();
    std::cout << "Level 1: After level 2 returned\n";

    std::cout << "Level 1: Before own async operation\n";
    co_await mock_async_op{};
    std::cout << "Level 1: After own async operation\n";
}

void test_3level_nested_coroutines()
{
    std::cout << "\n=== Test 4: 3-level nested coroutines ===\n";

    g_suspend_count = 0;
    g_resume_count = 0;

    mock_context ctx;
    g_work_queue = &ctx.work_queue_;
    auto ex = ctx.get_executor();

    boost::capy::async_run(ex)(level1_op());

    // With inline dispatch, coroutine runs until first suspend (level 3's async op)
    assert(g_suspend_count == 1);
    assert(g_resume_count == 0);
    std::cout << "After async_run, suspend: " << g_suspend_count << ", resume: " << g_resume_count
              << " (expected 1, 0)\n";

    ctx.run();

    // Should have performed 3 operations total (one at each level)
    assert(g_suspend_count == 3);
    assert(g_resume_count == 3);
    std::cout << "After ctx.run(), suspend: " << g_suspend_count << ", resume: " << g_resume_count
              << " (expected 3, 3)\n";

    std::cout << "Test passed!\n";
}

//------------------------------------------------
// Test: 3-level nesting with multiple operations at each level
//------------------------------------------------

boost::capy::task<> level3_multi(int ops)
{
    std::cout << "  Level 3: Performing " << ops << " operations\n";
    for(int i = 0; i < ops; ++i)
    {
        co_await mock_async_op{};
    }
    std::cout << "  Level 3: Completed " << ops << " operations\n";
}

boost::capy::task<> level2_multi()
{
    std::cout << " Level 2: Before calling level 3 (2 ops)\n";
    co_await level3_multi(2);
    std::cout << " Level 2: After level 3, doing own ops (3 ops)\n";
    for(int i = 0; i < 3; ++i)
    {
        co_await mock_async_op{};
    }
    std::cout << " Level 2: Completed own operations\n";
}

boost::capy::task<> level1_multi()
{
    std::cout << "Level 1: Before calling level 2\n";
    co_await level2_multi();
    std::cout << "Level 1: After level 2, doing own ops (4 ops)\n";
    for(int i = 0; i < 4; ++i)
    {
        co_await mock_async_op{};
    }
    std::cout << "Level 1: Completed own operations\n";
}

void test_3level_nested_multi_ops()
{
    std::cout << "\n=== Test 5: 3-level nested with multiple operations ===\n";

    g_suspend_count = 0;
    g_resume_count = 0;

    mock_context ctx;
    g_work_queue = &ctx.work_queue_;
    auto ex = ctx.get_executor();

    // Expected: 2 ops (level3) + 3 ops (level2) + 4 ops (level1) = 9 total
    boost::capy::async_run(ex)(level1_multi());

    // With inline dispatch, coroutine runs until first suspend
    assert(g_suspend_count == 1);
    assert(g_resume_count == 0);
    std::cout << "After async_run, suspend: " << g_suspend_count << ", resume: " << g_resume_count
              << " (expected 1, 0)\n";

    ctx.run();

    const int expected_ops = 2 + 3 + 4; // 9 total
    assert(g_suspend_count == expected_ops);
    assert(g_resume_count == expected_ops);
    std::cout << "After ctx.run(), suspend: " << g_suspend_count << ", resume: " << g_resume_count
              << " (expected " << expected_ops << ", " << expected_ops << ")\n";

    std::cout << "Test passed!\n";
}

//------------------------------------------------
// Test: 3-level nesting with same executor
//------------------------------------------------

boost::capy::task<> level3_shared()
{
    std::cout << "  Level 3: Async operation\n";
    co_await mock_async_op{};
}

boost::capy::task<> level2_shared()
{
    std::cout << " Level 2: Before level 3\n";
    co_await level3_shared();
    std::cout << " Level 2: After level 3, own operation\n";
    co_await mock_async_op{};
}

boost::capy::task<> level1_shared()
{
    std::cout << "Level 1: Before level 2\n";
    co_await level2_shared();
    std::cout << "Level 1: After level 2, own operation\n";
    co_await mock_async_op{};
}

void test_3level_shared_executor()
{
    std::cout << "\n=== Test 6: 3-level nested with shared executor ===\n";

    g_suspend_count = 0;
    g_resume_count = 0;

    mock_context ctx;
    g_work_queue = &ctx.work_queue_;
    auto ex = ctx.get_executor();

    boost::capy::async_run(ex)(level1_shared());

    // With inline dispatch, coroutine runs until first suspend
    assert(g_suspend_count == 1);
    assert(g_resume_count == 0);
    std::cout << "After async_run, suspend: " << g_suspend_count << ", resume: " << g_resume_count
              << " (expected 1, 0)\n";

    ctx.run();

    // 3 operations total (one at each level, all using same executor)
    assert(g_suspend_count == 3);
    assert(g_resume_count == 3);
    std::cout << "After ctx.run(), suspend: " << g_suspend_count << ", resume: " << g_resume_count
              << " (expected 3, 3)\n";

    std::cout << "Test passed!\n";
}

//------------------------------------------------
// Test: Stateful frame allocator
//------------------------------------------------

/** A stateful frame allocator that tracks allocations.

    This allocator maintains counters for allocations and deallocations
    to verify that the frame allocator is being used correctly.
*/
struct tracking_allocator
{
    struct stats
    {
        std::size_t alloc_count = 0;
        std::size_t dealloc_count = 0;
        std::size_t bytes_allocated = 0;
        std::size_t bytes_deallocated = 0;
    };

    stats* stats_;

    explicit tracking_allocator(stats* s)
        : stats_(s)
    {
    }

    void* allocate(std::size_t n)
    {
        ++stats_->alloc_count;
        stats_->bytes_allocated += n;
        return ::operator new(n);
    }

    void deallocate(void* p, std::size_t n)
    {
        ++stats_->dealloc_count;
        stats_->bytes_deallocated += n;
        ::operator delete(p);
    }
};

static_assert(boost::capy::frame_allocator<tracking_allocator>);

void test_stateful_allocator()
{
    std::cout << "\n=== Test 7: Stateful frame allocator ===\n";

    g_suspend_count = 0;
    g_resume_count = 0;

    tracking_allocator::stats stats;
    tracking_allocator alloc(&stats);

    mock_context ctx;
    g_work_queue = &ctx.work_queue_;
    auto ex = ctx.get_executor();

    std::cout << "Before async_run:\n";
    std::cout << "  alloc_count: " << stats.alloc_count << "\n";
    std::cout << "  dealloc_count: " << stats.dealloc_count << "\n";

    boost::capy::async_run(ex, alloc)(async_op_once());

    std::cout << "After async_run (before ctx.run()):\n";
    std::cout << "  alloc_count: " << stats.alloc_count << "\n";
    std::cout << "  dealloc_count: " << stats.dealloc_count << "\n";

    // Should have allocated at least 2 frames:
    // 1. The user's coroutine (async_op_once)
    // 2. The async_run_task internal coroutine
    assert(stats.alloc_count >= 2);
    assert(stats.dealloc_count == 0); // Nothing deallocated yet

    ctx.run();

    std::cout << "After ctx.run():\n";
    std::cout << "  alloc_count: " << stats.alloc_count << "\n";
    std::cout << "  dealloc_count: " << stats.dealloc_count << "\n";
    std::cout << "  bytes_allocated: " << stats.bytes_allocated << "\n";
    std::cout << "  bytes_deallocated: " << stats.bytes_deallocated << "\n";

    // All frames should be deallocated
    assert(stats.alloc_count == stats.dealloc_count);
    assert(stats.bytes_allocated == stats.bytes_deallocated);

    std::cout << "Test passed!\n";
}

//------------------------------------------------
// Test: Stateful allocator with nested coroutines
//------------------------------------------------

void test_stateful_allocator_nested()
{
    std::cout << "\n=== Test 8: Stateful allocator with 3-level nesting ===\n";

    g_suspend_count = 0;
    g_resume_count = 0;

    tracking_allocator::stats stats;
    tracking_allocator alloc(&stats);

    mock_context ctx;
    g_work_queue = &ctx.work_queue_;
    auto ex = ctx.get_executor();

    boost::capy::async_run(ex, alloc)(level1_op());

    std::cout << "After async_run (before ctx.run()):\n";
    std::cout << "  alloc_count: " << stats.alloc_count << "\n";

    // Should have allocated frames for:
    // 1. async_run_task
    // 2. level1_op
    // 3. level2_op
    // 4. level3_op
    assert(stats.alloc_count >= 4);

    ctx.run();

    std::cout << "After ctx.run():\n";
    std::cout << "  alloc_count: " << stats.alloc_count << "\n";
    std::cout << "  dealloc_count: " << stats.dealloc_count << "\n";

    // All frames should be deallocated
    assert(stats.alloc_count == stats.dealloc_count);
    assert(stats.bytes_allocated == stats.bytes_deallocated);

    std::cout << "Test passed!\n";
}

//------------------------------------------------

int main()
{
    std::cout << "=== async_run Tests ===\n";
    std::cout << "Testing coroutine launching and nesting with mock async operations\n\n";

    test_single_layer_coroutine();
    test_multiple_operations();
    test_multiple_coroutines();

    std::cout << "\n=== 3-Level Nested Coroutine Tests ===\n";
    test_3level_nested_coroutines();
    test_3level_nested_multi_ops();
    test_3level_shared_executor();

    std::cout << "\n=== Stateful Frame Allocator Tests ===\n";
    test_stateful_allocator();
    test_stateful_allocator_nested();

    std::cout << "\n=== All tests passed! ===\n";
    return 0;
}
