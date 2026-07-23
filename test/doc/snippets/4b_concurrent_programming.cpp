//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4b.concurrent-programming.adoc.

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

#include <boost/corosio/delay.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test_suite.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

using namespace std::chrono_literals;

void use_resource() {}

// tag::mutex_protection[]
std::mutex m;
int counter = 0;

void increment()
{
    std::lock_guard lock(m);
    ++counter; // Safe: only one thread at a time
}
// end::mutex_protection[]

bool stopped = false;
int events_handled = 0;

// Each simulated event arms the next until the source runs dry.
void wait_for_event() {}

void handle_event()
{
    if (++events_handled == 3)
        stopped = true;
}

void
run_event_loop()
{
    // tag::event_loop[]
    while (!stopped)
    {
        wait_for_event();     // Blocks until I/O completes
        handle_event();       // Run the handler
    }
    // end::event_loop[]
}

// tag::suspension_points[]
capy::task<void> handle_client(corosio::tcp_socket sock)
{
    char buf[1024];

    auto [ec, n] = co_await sock.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));
    // Suspends here until data arrives

    if (ec)
        co_return;  // Exit on error

    // Process data...
}
// end::suspension_points[]

capy::task<void> my_coroutine() { co_return; }

char session_storage[64];
capy::mutable_buffer buf(session_storage, sizeof(session_storage));
capy::const_buffer response("OK", 2);

// tag::strand_session[]
capy::task<void> session(corosio::tcp_socket sock)
{
    // All code in this coroutine runs sequentially
    auto [ec, n] = co_await sock.read_some(buf);
    // No other code in this coroutine runs until above completes

    co_await sock.write_some(response);
    // Still sequential
}
// end::strand_session[]

// tag::accept_loop[]
capy::task<void> accept_loop(
    corosio::io_context& ioc,
    corosio::tcp_acceptor& acc)
{
    for (;;)
    {
        corosio::tcp_socket peer(ioc);
        auto [ec] = co_await acc.accept(peer);
        if (ec) break;

        // Spawn independent coroutine for this client
        capy::run_async(ioc.get_executor())(
            handle_client(std::move(peer)));
    }
}
// end::accept_loop[]

// Accepting needs a live listening socket; the loop compiles but
// never runs.
[[maybe_unused]] capy::task<void> (* const accept_loop_demo)(
    corosio::io_context&, corosio::tcp_acceptor&) = &accept_loop;

// tag::worker_pool[]
struct worker
{
    corosio::tcp_socket sock;
    std::string buf;
    bool in_use = false;

    explicit worker(corosio::io_context& ioc) : sock(ioc) {}
};
// end::worker_pool[]

std::size_t
make_worker_pool(corosio::io_context& ioc, int max_workers)
{
    // tag::worker_pool_use[]
    // Preallocate workers
    std::vector<worker> workers;
    workers.reserve(max_workers);
    for (int i = 0; i < max_workers; ++i)
        workers.emplace_back(ioc);

    // Assign connections to free workers
    // end::worker_pool_use[]
    return workers.size();
}

capy::task<std::string> read_message(corosio::tcp_socket&)
{
    co_return "request";
}

capy::task<std::string> process(std::string const&)
{
    co_return "response";
}

capy::task<void> write_response(corosio::tcp_socket&, std::string const&)
{
    co_return;
}

// tag::pipeline[]
capy::task<void> pipeline(corosio::tcp_socket sock)
{
    auto message = co_await read_message(sock);
    auto result = co_await process(message);
    co_await write_response(sock, result);
}
// end::pipeline[]

// tag::blocking[]
// WRONG: blocks the entire io_context
capy::task<void> bad()
{
    std::this_thread::sleep_for(1s);  // Don't do this!
    co_return;
}

// RIGHT: suspend with an async delay
capy::task<void> good()
{
    co_await corosio::delay(1s);
}
// end::blocking[]

// Running either coroutine costs a full second of wall clock; the
// bodies are the demonstration, so nothing awaits them.
[[maybe_unused]] capy::task<void> (* const bad_demo)() = &bad;
[[maybe_unused]] capy::task<void> (* const good_demo)() = &good;

capy::task<> use_socket(corosio::tcp_socket&) { co_return; }

// The rvalue overload moves the socket into the frame so the RIGHT
// variant genuinely owns it; a by-value overload would make the
// lvalue call above ambiguous.
capy::task<> use_socket(corosio::tcp_socket&& s)
{
    auto owned = std::move(s);
    co_return;
}

// Both scopes spawn coroutines that would outlive the demo; the
// scoping bug is the lesson, so nothing calls this.
[[maybe_unused]] void
dangling_reference(corosio::io_context& ioc, capy::executor_ref ex)
{
    // tag::dangling_reference[]
    // WRONG: socket destroyed while coroutine runs
    {
        corosio::tcp_socket sock(ioc);
        capy::run_async(ex)(use_socket(sock));  // Takes reference!
    }  // sock destroyed here, coroutine still running

    // RIGHT: move socket into coroutine
    {
        corosio::tcp_socket sock(ioc);
        capy::run_async(ex)(use_socket(std::move(sock)));
    }  // OK, coroutine owns the socket
    // end::dangling_reference[]
}

// Connecting from the wrong executor is the bug on display; the
// function compiles but never runs.
[[maybe_unused]] void
cross_executor(
    corosio::io_context& ctx1, capy::executor_ref ex2, corosio::endpoint ep)
{
    // tag::cross_executor[]
    // Dangerous: socket created on ctx1, used from ex2
    corosio::tcp_socket sock(ctx1);
    capy::run_async(ex2)([&sock, ep]() -> capy::task<void> {
        co_await sock.connect(ep);  // Wrong executor!
    }());
    // end::cross_executor[]
}

capy::task<>
feed(corosio::tcp_socket& s)
{
    static char const ping[] = {'p', 'i', 'n', 'g'};
    co_await capy::write(s, capy::const_buffer(ping, sizeof(ping)));
}

struct concurrent_programming_test
{
    void
    testRaceCondition()
    {
        // tag::race_condition[]
        int counter = 0;

        // Task 1                  // Task 2
        ++counter;                 ++counter;
        // Both read 0, both write 1
        // Expected: 2, Actual: 1 (data race)
        // end::race_condition[]

        // Sequential execution here; the comments describe the
        // concurrent interleaving.
        BOOST_TEST(counter == 2);
    }

    void
    testReadModifyWrite()
    {
        bool resource_available = true;
        // tag::read_modify_write[]
        if (resource_available)      // Read
        {
            resource_available = false; // Write
            use_resource();
        }
        // end::read_modify_write[]
        BOOST_TEST(!resource_available);
    }

    void
    testMutexProtection()
    {
        increment();
        BOOST_TEST(counter == 1);
    }

    void
    testEventLoop()
    {
        run_event_loop();
        BOOST_TEST(events_handled == 3);
    }

    void
    testHandleClient()
    {
        corosio::io_context ioc;
        auto [s1, s2] = corosio::test::make_socket_pair(ioc);
        auto ex = ioc.get_executor();
        bool done = false;
        capy::run_async(ex)(feed(s2));
        capy::run_async(ex)(
            [](corosio::tcp_socket s, bool& out) -> capy::task<>
            {
                co_await handle_client(std::move(s));
                out = true;
            }(std::move(s1), done));
        ioc.run();
        BOOST_TEST(done);
    }

    void
    testExecutorAffinity()
    {
        corosio::io_context ioc;
        // tag::executor_affinity[]
        capy::run_async(ioc.get_executor())(my_coroutine());
        // my_coroutine resumes through ioc's executor
        // end::executor_affinity[]
        BOOST_TEST(ioc.run() > 0u);
    }

    void
    testSession()
    {
        corosio::io_context ioc;
        auto [s1, s2] = corosio::test::make_socket_pair(ioc);
        auto ex = ioc.get_executor();
        bool done = false;
        capy::run_async(ex)(feed(s2));
        capy::run_async(ex)(
            [](corosio::tcp_socket s, bool& out) -> capy::task<>
            {
                co_await session(std::move(s));
                out = true;
            }(std::move(s1), done));
        ioc.run();
        BOOST_TEST(done);
    }

    void
    testMultiThreadedRun()
    {
        // tag::multi_threaded_run[]
        corosio::io_context ioc(4);  // Hint: 4 threads

        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i)
            threads.emplace_back([&ioc] { ioc.run(); });

        for (auto& t : threads)
            t.join();
        // end::multi_threaded_run[]
        BOOST_TEST(threads.size() == 4u);
    }

    void
    testWorkerPool()
    {
        corosio::io_context ioc;
        BOOST_TEST(make_worker_pool(ioc, 4) == 4u);
    }

    void
    testPipeline()
    {
        corosio::io_context ioc;
        corosio::tcp_socket sock(ioc);
        bool done = false;
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket s, bool& out) -> capy::task<>
            {
                co_await pipeline(std::move(s));
                out = true;
            }(std::move(sock), done));
        ioc.run();
        BOOST_TEST(done);
    }

    void
    run()
    {
        testRaceCondition();
        testReadModifyWrite();
        testMutexProtection();
        testEventLoop();
        testHandleClient();
        testExecutorAffinity();
        testSession();
        testMultiThreadedRun();
        testWorkerPool();
        testPipeline();
    }
};

} // namespace

TEST_SUITE(
    concurrent_programming_test,
    "boost.corosio.doc.4b_concurrent_programming");
