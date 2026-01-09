//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Comprehensive tests for capy::task
// Tests all flow diagrams from context/design.md

#include <capy/task.hpp>
#include <capy/async_run.hpp>
#include <capy/run_on.hpp>
#include <capy/executor.hpp>

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

//------------------------------------------------------------------------------
// Test infrastructure
//------------------------------------------------------------------------------

// Overloaded helper for creating handlers with multiple overloads
template<class... Ts>
struct overloaded : Ts...
{
    using Ts::operator()...;
};
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

// Track execution order
std::vector<std::string> execution_log;

void log(std::string const& msg)
{
    execution_log.push_back(msg);
    std::cout << "  " << msg << "\n";
}

void clear_log()
{
    execution_log.clear();
}

void expect_log(std::initializer_list<std::string> expected)
{
    std::vector<std::string> exp(expected);
    if(execution_log != exp)
    {
        std::cerr << "Log mismatch!\n";
        std::cerr << "Expected:\n";
        for(auto const& s : exp)
            std::cerr << "  " << s << "\n";
        std::cerr << "Got:\n";
        for(auto const& s : execution_log)
            std::cerr << "  " << s << "\n";
        assert(false && "Log mismatch");
    }
}

//------------------------------------------------------------------------------
// Mock executor for testing
//------------------------------------------------------------------------------

struct test_executor : capy::executor_base
{
    std::string name_;
    mutable int dispatch_count_ = 0;
    mutable std::vector<std::string> dispatches_;

    explicit test_executor(std::string name)
        : name_(std::move(name))
    {
    }

    capy::coro dispatch(capy::coro h) const override
    {
        ++dispatch_count_;
        dispatches_.push_back(name_ + ".dispatch");
        log(name_ + ".dispatch");
        return h;
    }

    void post(capy::executor_work* w) const override
    {
        log(name_ + ".post");
        (*w)();
    }

    // Required by executor concept
    test_executor(test_executor const&) = default;
    test_executor& operator=(test_executor const&) = default;
    test_executor(test_executor&&) = default;
    test_executor& operator=(test_executor&&) = default;
};

static_assert(capy::executor<test_executor>);

//------------------------------------------------------------------------------
// Mock io_object for testing dispatcher propagation
//------------------------------------------------------------------------------

struct mock_io_op
{
    std::string name_;
    capy::any_dispatcher captured_dispatcher_;
    capy::coro continuation_;

    explicit mock_io_op(std::string name)
        : name_(std::move(name))
    {
    }

    bool await_ready() const noexcept
    {
        return false;
    }

    void await_resume()
    {
        log(name_ + ".await_resume");
    }

    // Affine awaitable protocol
    template<capy::dispatcher D>
    capy::coro await_suspend(capy::coro h, D const& d)
    {
        log(name_ + ".await_suspend");
        captured_dispatcher_ = d;
        continuation_ = h;
        // Simulate immediate completion
        return d(h);
    }
};

static_assert(capy::affine_awaitable<mock_io_op, capy::any_dispatcher>);

//------------------------------------------------------------------------------
// Flow diagram tests from design.md
//------------------------------------------------------------------------------

// Flow: c -> io
// Simple coroutine awaiting io_object
capy::task<> flow_c_io(mock_io_op& io)
{
    log("c.start");
    co_await io;
    log("c.end");
}

void test_flow_c_io()
{
    std::cout << "=== Test: c -> io ===\n";
    clear_log();

    test_executor ex("ex1");
    mock_io_op io("io");

    bool completed = false;
    capy::async_run(ex)(flow_c_io(io), overloaded{
        [&]() {
            completed = true;
            log("completed");
        },
        [](std::exception_ptr ep) {
            if(ep)
                std::rethrow_exception(ep);
        }
    });

    assert(completed);
    expect_log({
        "ex1.dispatch",  // async_run dispatches to start
        "c.start",
        "io.await_suspend",
        "ex1.dispatch",  // io completes through dispatcher
        "io.await_resume",
        "c.end",
        "completed"
    });

    std::cout << "  PASSED\n\n";
}

// Flow: c1 -> c2 -> io
// Chained coroutines
capy::task<> flow_c2(mock_io_op& io)
{
    log("c2.start");
    co_await io;
    log("c2.end");
}

capy::task<> flow_c1_c2_io(mock_io_op& io)
{
    log("c1.start");
    co_await flow_c2(io);
    log("c1.end");
}

void test_flow_c1_c2_io()
{
    std::cout << "=== Test: c1 -> c2 -> io ===\n";
    clear_log();

    test_executor ex("ex1");
    mock_io_op io("io");

    bool completed = false;
    capy::async_run(ex)(flow_c1_c2_io(io), overloaded{
        [&]() {
            completed = true;
            log("completed");
        },
        [](std::exception_ptr ep) {
            if(ep)
                std::rethrow_exception(ep);
        }
    });

    assert(completed);
    // c2 returns to c1 with same dispatcher -> symmetric transfer (no dispatch)
    expect_log({
        "ex1.dispatch",  // async_run dispatches to start
        "c1.start",
        "c2.start",
        "io.await_suspend",
        "ex1.dispatch",  // io completes through dispatcher
        "io.await_resume",
        "c2.end",
        // c2 -> c1 is symmetric transfer (same dispatcher, no dispatch call)
        "c1.end",
        "completed"
    });

    std::cout << "  PASSED\n\n";
}

// Flow: c1 -> c2 -> io with return value
capy::task<int> flow_c2_value(mock_io_op& io)
{
    log("c2.start");
    co_await io;
    log("c2.end");
    co_return 42;
}

capy::task<int> flow_c1_c2_value(mock_io_op& io)
{
    log("c1.start");
    int result = co_await flow_c2_value(io);
    log("c1.end");
    co_return result * 2;
}

void test_flow_with_return_value()
{
    std::cout << "=== Test: c1 -> c2 -> io (with return value) ===\n";
    clear_log();

    test_executor ex("ex1");
    mock_io_op io("io");

    int final_result = 0;
    capy::async_run(ex)(flow_c1_c2_value(io), overloaded{
        [&](int result) {
            final_result = result;
            log("completed with " + std::to_string(result));
        },
        [](std::exception_ptr ep) {
            if(ep)
                std::rethrow_exception(ep);
        }
    });

    assert(final_result == 84);
    std::cout << "  Result: " << final_result << "\n";
    std::cout << "  PASSED\n\n";
}

// Flow: !c1 -> c2 -> !c3 -> io
// Executor changes mid-chain
capy::task<> flow_c3_ex2(mock_io_op& io)
{
    log("c3.start");
    co_await io;
    log("c3.end");
}

capy::task<> flow_c2_changes_executor(mock_io_op& io, test_executor& ex2)
{
    log("c2.start");
    co_await capy::run_on(ex2, flow_c3_ex2(io));
    log("c2.end");
}

capy::task<> flow_c1_ex1(mock_io_op& io, test_executor& ex2)
{
    log("c1.start");
    co_await flow_c2_changes_executor(io, ex2);
    log("c1.end");
}

void test_flow_executor_change()
{
    std::cout << "=== Test: !c1 -> c2 -> !c3 -> io ===\n";
    std::cout << "  (executor change mid-chain with symmetric transfer)\n";
    clear_log();

    test_executor ex1("ex1");
    test_executor ex2("ex2");
    mock_io_op io("io");

    bool completed = false;
    capy::async_run(ex1)(flow_c1_ex1(io, ex2), overloaded{
        [&]() {
            completed = true;
            log("completed");
        },
        [](std::exception_ptr ep) {
            if(ep)
                std::rethrow_exception(ep);
        }
    });

    assert(completed);

    // Verify dispatcher propagation:
    // - c1 launched on ex1
    // - c2 continues on ex1 (inherited)
    // - c3 launched on ex2 (via run_on)
    // - io captures ex2's dispatcher
    // - c3 returns to c2 through ex1 (caller_ex)
    // - c2 returns to c1 symmetrically (same ex1 dispatcher)
    expect_log({
        "ex1.dispatch",  // async_run starts c1
        "c1.start",
        "c2.start",
        "c3.start",
        "io.await_suspend",
        "ex2.dispatch",  // io completes through c3's dispatcher (ex2)
        "io.await_resume",
        "c3.end",
        "ex1.dispatch",  // c3 returns to c2 through ex1 (different dispatcher)
        "c2.end",
        // c2 -> c1 is symmetric transfer (same ex1 dispatcher, no dispatch)
        "c1.end",
        "completed"
    });

    std::cout << "  ex1 dispatch count: " << ex1.dispatch_count_ << "\n";
    std::cout << "  ex2 dispatch count: " << ex2.dispatch_count_ << "\n";
    std::cout << "  PASSED\n\n";
}

// Test: Same-executor symmetric transfer optimization
void test_same_executor_symmetric_transfer()
{
    std::cout << "=== Test: Same-executor symmetric transfer ===\n";
    std::cout << "  (verifies no dispatch call when caller == callee dispatcher)\n";
    clear_log();

    test_executor ex("ex");
    mock_io_op io("io");

    // Deep chain with same executor throughout
    auto c4 = [&]() -> capy::task<> {
        log("c4.start");
        co_await io;
        log("c4.end");
    };

    auto c3 = [&]() -> capy::task<> {
        log("c3.start");
        co_await c4();
        log("c3.end");
    };

    auto c2 = [&]() -> capy::task<> {
        log("c2.start");
        co_await c3();
        log("c2.end");
    };

    auto c1 = [&]() -> capy::task<> {
        log("c1.start");
        co_await c2();
        log("c1.end");
    };

    bool completed = false;
    capy::async_run(ex)(c1(), overloaded{
        [&]() {
            completed = true;
        },
        [](std::exception_ptr ep) {
            if(ep)
                std::rethrow_exception(ep);
        }
    });

    assert(completed);

    // Verify symmetric transfer by checking log pattern:
    // After io.await_resume, all coroutine ends should happen consecutively
    // without any dispatch calls between them.
    // Expected: ex.dispatch, c1.start, ..., io.await_resume, c4.end, c3.end, c2.end, c1.end
    // If symmetric transfer failed, we'd see: io.await_resume, c4.end, ex.dispatch, c3.end, ...
    expect_log({
        "ex.dispatch",      // async_run dispatches to start
        "c1.start",
        "c2.start",
        "c3.start",
        "c4.start",
        "io.await_suspend",
        "ex.dispatch",      // io completion dispatches back
        "io.await_resume",
        "c4.end",           // All these happen via symmetric transfer
        "c3.end",           // (no dispatch between them)
        "c2.end",
        "c1.end"
    });

    std::cout << "  PASSED\n\n";
}

// Test: Dispatcher identity comparison
void test_dispatcher_identity()
{
    std::cout << "=== Test: Dispatcher identity comparison ===\n";

    test_executor ex1("ex1");
    test_executor ex2("ex2");

    capy::any_dispatcher d1a = ex1;
    capy::any_dispatcher d1b = ex1;
    capy::any_dispatcher d2 = ex2;

    // Same executor -> same dispatcher identity
    assert(d1a == d1b);
    std::cout << "  d1a == d1b: true (same executor)\n";

    // Different executors -> different dispatcher identity
    assert(!(d1a == d2));
    std::cout << "  d1a == d2: false (different executors)\n";

    std::cout << "  PASSED\n\n";
}

// Test: Task move semantics
void test_task_move()
{
    std::cout << "=== Test: Task move semantics ===\n";

    auto make_task = []() -> capy::task<int> {
        co_return 42;
    };

    capy::task<int> t1 = make_task();
    capy::task<int> t2 = std::move(t1);

    // t1 should be empty after move
    // t2 should have the coroutine

    test_executor ex("ex");
    int result = 0;
    capy::async_run(ex)(std::move(t2), overloaded{
        [&](int r) {
            result = r;
        },
        [](std::exception_ptr ep) {
            if(ep)
                std::rethrow_exception(ep);
        }
    });

    assert(result == 42);
    std::cout << "  Move semantics working correctly\n";
    std::cout << "  PASSED\n\n";
}

// Test: Exception handling via handler
void test_exception_handling()
{
    std::cout << "=== Test: Exception handling ===\n";

    auto throwing_task = []() -> capy::task<> {
        throw std::runtime_error("test error");
        co_return;
    };

    test_executor ex("ex");
    bool caught = false;
    std::string error_msg;

    capy::async_run(ex)(throwing_task(), overloaded{
        [&]() {
            // Should not reach here
        },
        [&](std::exception_ptr ep) {
            if(ep)
            {
                try
                {
                    std::rethrow_exception(ep);
                }
                catch(std::runtime_error const& e)
                {
                    caught = true;
                    error_msg = e.what();
                }
            }
        }
    });

    assert(caught);
    assert(error_msg == "test error");
    std::cout << "  Exception caught: " << error_msg << "\n";
    std::cout << "  PASSED\n\n";
}

// Test: Void vs non-void task
void test_void_and_nonvoid_tasks()
{
    std::cout << "=== Test: Void and non-void tasks ===\n";

    // Void task
    auto void_task = []() -> capy::task<> {
        co_return;
    };

    // Int task
    auto int_task = []() -> capy::task<int> {
        co_return 123;
    };

    // String task
    auto string_task = []() -> capy::task<std::string> {
        co_return "hello";
    };

    test_executor ex("ex");

    // Test void
    bool void_done = false;
    capy::async_run(ex)(void_task(), overloaded{
        [&]() {
            void_done = true;
        },
        [](std::exception_ptr ep) {
            if(ep)
                std::rethrow_exception(ep);
        }
    });
    assert(void_done);
    std::cout << "  void task: OK\n";

    // Test int
    int int_result = 0;
    capy::async_run(ex)(int_task(), overloaded{
        [&](int r) {
            int_result = r;
        },
        [](std::exception_ptr ep) {
            if(ep)
                std::rethrow_exception(ep);
        }
    });
    assert(int_result == 123);
    std::cout << "  int task: " << int_result << "\n";

    // Test string
    std::string str_result;
    capy::async_run(ex)(string_task(), overloaded{
        [&](std::string r) {
            str_result = std::move(r);
        },
        [](std::exception_ptr ep) {
            if(ep)
                std::rethrow_exception(ep);
        }
    });
    assert(str_result == "hello");
    std::cout << "  string task: " << str_result << "\n";

    std::cout << "  PASSED\n\n";
}

//------------------------------------------------------------------------------
// Main
//------------------------------------------------------------------------------

int main()
{
    std::cout << "========================================\n";
    std::cout << "capy::task comprehensive tests\n";
    std::cout << "Testing flow diagrams from design.md\n";
    std::cout << "========================================\n\n";

    test_dispatcher_identity();
    test_flow_c_io();
    test_flow_c1_c2_io();
    test_flow_with_return_value();
    test_flow_executor_change();
    test_same_executor_symmetric_transfer();
    test_task_move();
    test_exception_handling();
    test_void_and_nonvoid_tasks();

    std::cout << "========================================\n";
    std::cout << "All tests passed!\n";
    std::cout << "========================================\n";

    return 0;
}
