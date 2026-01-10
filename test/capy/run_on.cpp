//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Tests for boost::capy::run_on
// Tests flow diagrams from context/design.md involving executor changes

#include <boost/capy/run_on.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/async_run.hpp>
#include <boost/capy/executor.hpp>

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

//------------------------------------------------------------------------------
// Test infrastructure
//------------------------------------------------------------------------------

template<class... Ts>
struct overloaded : Ts...
{
    using Ts::operator()...;
};
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

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

struct test_executor : boost::capy::executor_base
{
    std::string name_;
    mutable int dispatch_count_ = 0;

    explicit test_executor(std::string name)
        : name_(std::move(name))
    {
    }

    boost::capy::coro dispatch(boost::capy::coro h) const override
    {
        ++dispatch_count_;
        log(name_ + ".dispatch");
        return h;
    }

    void post(boost::capy::executor_work* w) const override
    {
        log(name_ + ".post");
        (*w)();
    }

    test_executor(test_executor const&) = default;
    test_executor& operator=(test_executor const&) = default;
    test_executor(test_executor&&) = default;
    test_executor& operator=(test_executor&&) = default;
};

static_assert(boost::capy::executor<test_executor>);

//------------------------------------------------------------------------------
// Mock io_object for testing dispatcher propagation
//------------------------------------------------------------------------------

struct mock_io_op
{
    std::string name_;
    boost::capy::any_dispatcher captured_dispatcher_;

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

    template<boost::capy::dispatcher D>
    boost::capy::coro await_suspend(boost::capy::coro h, D const& d)
    {
        log(name_ + ".await_suspend");
        captured_dispatcher_ = d;
        return d(h);
    }
};

static_assert(boost::capy::affine_awaitable<mock_io_op, boost::capy::any_dispatcher>);

//------------------------------------------------------------------------------
// Test: Basic run_on functionality
//------------------------------------------------------------------------------

// Flow: !c -> io (c runs on ex2)
boost::capy::task<> basic_task(mock_io_op& io)
{
    log("c.start");
    co_await io;
    log("c.end");
}

void test_basic_run_on()
{
    std::cout << "=== Test: Basic run_on ===\n";
    clear_log();

    test_executor ex1("ex1");
    test_executor ex2("ex2");
    mock_io_op io("io");

    // Outer task runs on ex1, but uses run_on to execute inner on ex2
    auto outer = [&]() -> boost::capy::task<> {
        log("outer.start");
        co_await boost::capy::run_on(ex2, basic_task(io));
        log("outer.end");
    };

    bool completed = false;
    boost::capy::async_run(ex1)(outer(), [&]() {
        completed = true;
        log("completed");
    });

    assert(completed);
    expect_log({
        "ex1.dispatch",     // async_run starts outer
        "outer.start",
        "c.start",          // run_on starts inner task
        "io.await_suspend",
        "ex2.dispatch",     // io completes through ex2 (run_on's executor)
        "io.await_resume",
        "c.end",
        "ex1.dispatch",     // run_on returns to outer through ex1 (caller's executor)
        "outer.end",
        "completed"
    });

    std::cout << "  ex1 dispatch count: " << ex1.dispatch_count_ << "\n";
    std::cout << "  ex2 dispatch count: " << ex2.dispatch_count_ << "\n";
    std::cout << "  PASSED\n\n";
}

//------------------------------------------------------------------------------
// Test: Flow diagram !c1 -> c2 -> !c3 -> io from design.md
//------------------------------------------------------------------------------

boost::capy::task<> flow_c3(mock_io_op& io)
{
    log("c3.start");
    co_await io;
    log("c3.end");
}

boost::capy::task<> flow_c2(mock_io_op& io, test_executor& ex2)
{
    log("c2.start");
    co_await boost::capy::run_on(ex2, flow_c3(io));
    log("c2.end");
}

boost::capy::task<> flow_c1(mock_io_op& io, test_executor& ex2)
{
    log("c1.start");
    co_await flow_c2(io, ex2);
    log("c1.end");
}

void test_flow_executor_change_mid_chain()
{
    std::cout << "=== Test: !c1 -> c2 -> !c3 -> io (executor change mid-chain) ===\n";
    clear_log();

    test_executor ex1("ex1");
    test_executor ex2("ex2");
    mock_io_op io("io");

    bool completed = false;
    boost::capy::async_run(ex1)(flow_c1(io, ex2), [&]() {
        completed = true;
        log("completed");
    });

    assert(completed);

    // From design.md:
    // - c1 launched on ex1
    // - c2 continues on ex1 (inherited)
    // - c3 launched on ex2 (via run_on)
    // - io captures ex2
    // - c3 returns to c2 through ex1 (caller_ex)
    // - c2 returns to c1 symmetrically (same ex1)
    expect_log({
        "ex1.dispatch",     // async_run starts c1
        "c1.start",
        "c2.start",
        "c3.start",
        "io.await_suspend",
        "ex2.dispatch",     // io completes through ex2
        "io.await_resume",
        "c3.end",
        "ex1.dispatch",     // c3 returns to c2 through ex1
        "c2.end",
        // c2 -> c1 is symmetric transfer (same ex1, no dispatch)
        "c1.end",
        "completed"
    });

    std::cout << "  ex1 dispatch count: " << ex1.dispatch_count_ << "\n";
    std::cout << "  ex2 dispatch count: " << ex2.dispatch_count_ << "\n";
    std::cout << "  PASSED\n\n";
}

//------------------------------------------------------------------------------
// Test: run_on with return value
//------------------------------------------------------------------------------

boost::capy::task<int> value_task()
{
    log("value_task.start");
    co_return 42;
}

boost::capy::task<int> outer_value_task(test_executor& ex2)
{
    log("outer.start");
    int result = co_await boost::capy::run_on(ex2, value_task());
    log("outer.got_value");
    co_return result * 2;
}

void test_run_on_with_return_value()
{
    std::cout << "=== Test: run_on with return value ===\n";
    clear_log();

    test_executor ex1("ex1");
    test_executor ex2("ex2");

    int final_result = 0;
    boost::capy::async_run(ex1)(outer_value_task(ex2), [&](int r) {
        final_result = r;
        log("completed with " + std::to_string(r));
    });

    assert(final_result == 84);
    std::cout << "  Result: " << final_result << " (expected 84)\n";
    std::cout << "  PASSED\n\n";
}

//------------------------------------------------------------------------------
// Test: run_on with void task
//------------------------------------------------------------------------------

boost::capy::task<> void_task()
{
    log("void_task");
    co_return;
}

void test_run_on_void_task()
{
    std::cout << "=== Test: run_on with void task ===\n";
    clear_log();

    test_executor ex1("ex1");
    test_executor ex2("ex2");

    auto outer = [&]() -> boost::capy::task<> {
        log("outer.start");
        co_await boost::capy::run_on(ex2, void_task());
        log("outer.end");
    };

    bool completed = false;
    boost::capy::async_run(ex1)(outer(), [&]() {
        completed = true;
        log("completed");
    });

    assert(completed);
    std::cout << "  PASSED\n\n";
}

//------------------------------------------------------------------------------
// Test: Exception propagation through run_on
//------------------------------------------------------------------------------

boost::capy::task<> throwing_task()
{
    log("throwing_task");
    throw std::runtime_error("test error from run_on");
    co_return;
}

void test_run_on_exception_propagation()
{
    std::cout << "=== Test: Exception propagation through run_on ===\n";
    clear_log();

    test_executor ex1("ex1");
    test_executor ex2("ex2");

    auto outer = [&]() -> boost::capy::task<> {
        log("outer.start");
        co_await boost::capy::run_on(ex2, throwing_task());
        log("outer.end");  // Should not reach here
    };

    bool caught = false;
    std::string error_msg;

    boost::capy::async_run(ex1)(outer(), overloaded{
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
                    log("caught: " + error_msg);
                }
            }
        }
    });

    assert(caught);
    assert(error_msg == "test error from run_on");
    std::cout << "  Exception propagated: " << error_msg << "\n";
    std::cout << "  PASSED\n\n";
}

//------------------------------------------------------------------------------
// Test: Nested run_on calls
//------------------------------------------------------------------------------

boost::capy::task<> innermost_task(mock_io_op& io)
{
    log("innermost.start");
    co_await io;
    log("innermost.end");
}

void test_nested_run_on()
{
    std::cout << "=== Test: Nested run_on calls ===\n";
    clear_log();

    test_executor ex1("ex1");
    test_executor ex2("ex2");
    test_executor ex3("ex3");
    mock_io_op io("io");

    auto middle = [&]() -> boost::capy::task<> {
        log("middle.start");
        co_await boost::capy::run_on(ex3, innermost_task(io));
        log("middle.end");
    };

    auto outer = [&]() -> boost::capy::task<> {
        log("outer.start");
        co_await boost::capy::run_on(ex2, middle());
        log("outer.end");
    };

    bool completed = false;
    boost::capy::async_run(ex1)(outer(), [&]() {
        completed = true;
        log("completed");
    });

    assert(completed);

    // Verify executor chain:
    // outer on ex1 -> middle on ex2 -> innermost on ex3
    // io captures ex3, completes through ex3
    // innermost returns to middle through ex2
    // middle returns to outer through ex1
    expect_log({
        "ex1.dispatch",     // async_run starts outer
        "outer.start",
        "middle.start",
        "innermost.start",
        "io.await_suspend",
        "ex3.dispatch",     // io completes through ex3
        "io.await_resume",
        "innermost.end",
        "ex2.dispatch",     // innermost returns to middle through ex2
        "middle.end",
        "ex1.dispatch",     // middle returns to outer through ex1
        "outer.end",
        "completed"
    });

    std::cout << "  ex1 dispatch count: " << ex1.dispatch_count_ << "\n";
    std::cout << "  ex2 dispatch count: " << ex2.dispatch_count_ << "\n";
    std::cout << "  ex3 dispatch count: " << ex3.dispatch_count_ << "\n";
    std::cout << "  PASSED\n\n";
}

//------------------------------------------------------------------------------
// Test: run_on with same executor (redundant but valid)
//------------------------------------------------------------------------------

void test_run_on_same_executor()
{
    std::cout << "=== Test: run_on with same executor ===\n";
    clear_log();

    test_executor ex("ex");
    mock_io_op io("io");

    auto inner = [&]() -> boost::capy::task<> {
        log("inner.start");
        co_await io;
        log("inner.end");
    };

    auto outer = [&]() -> boost::capy::task<> {
        log("outer.start");
        // run_on with same executor - redundant but should work
        co_await boost::capy::run_on(ex, inner());
        log("outer.end");
    };

    bool completed = false;
    boost::capy::async_run(ex)(outer(), [&]() {
        completed = true;
        log("completed");
    });

    assert(completed);
    std::cout << "  PASSED\n\n";
}

//------------------------------------------------------------------------------
// Test: run_on preserves dispatcher for io operations
//------------------------------------------------------------------------------

void test_run_on_dispatcher_propagation()
{
    std::cout << "=== Test: run_on dispatcher propagation to io ===\n";
    clear_log();

    test_executor ex1("ex1");
    test_executor ex2("ex2");
    mock_io_op io("io");

    auto inner = [&]() -> boost::capy::task<> {
        co_await io;
    };

    auto outer = [&]() -> boost::capy::task<> {
        co_await boost::capy::run_on(ex2, inner());
    };

    boost::capy::async_run(ex1)(outer(), [&]() {});

    // Verify io captured ex2's dispatcher (run_on's executor)
    // The dispatch happened through ex2
    assert(ex2.dispatch_count_ == 1);
    std::cout << "  io was dispatched through ex2 (count: " << ex2.dispatch_count_ << ")\n";
    std::cout << "  PASSED\n\n";
}

//------------------------------------------------------------------------------
// Main
//------------------------------------------------------------------------------

int main()
{
    std::cout << "========================================\n";
    std::cout << "boost::capy::run_on tests\n";
    std::cout << "Testing executor binding from design.md\n";
    std::cout << "========================================\n\n";

    test_basic_run_on();
    test_flow_executor_change_mid_chain();
    test_run_on_with_return_value();
    test_run_on_void_task();
    test_run_on_exception_propagation();
    test_nested_run_on();
    test_run_on_same_executor();
    test_run_on_dispatcher_propagation();

    std::cout << "========================================\n";
    std::cout << "All tests passed!\n";
    std::cout << "========================================\n";

    return 0;
}
