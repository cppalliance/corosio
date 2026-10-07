//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Acceptance gate for the object_pool conversion: steady-state
// construct()/destroy() and the accept/connect/read/write dispatch
// path must not call the global allocator once the per-service free
// list has been warmed. Counts against alloc_counter.hpp's shared
// interposer -- see that header for why this TU does not define its
// own operator new/delete.
//
// Excluded under ASan: ASan instruments its own replacement of the
// global allocation functions, and a second counting layer on top of
// that does not reliably observe the same calls. Excluded under TSan
// for the same reason alloc_counter.hpp's counters don't exist there
// (its runtime replaces operator new/delete itself).
#include "context.hpp"       // COROSIO_TEST_HAS_ASAN
#include "alloc_counter.hpp" // COROSIO_TEST_HAS_TSAN, alloc_armed/alloc_count

#if !COROSIO_TEST_HAS_ASAN && !defined(COROSIO_TEST_HAS_TSAN)

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/delay.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <type_traits>

#include "test_suite.hpp"

namespace boost::corosio {

namespace {

// Cycles warmed with counting off, before the assertion window opens.
constexpr int warmup_cycles = 8;

// Cycles measured with counting on.
constexpr int measured_cycles = 8;

} // namespace

template<auto Backend>
struct zero_alloc_test
{
    // One accept -> echo one buffer -> close round trip. `server` is
    // constructed and destroyed by the caller on every cycle -- that
    // construct()/destroy() pair is the thing under test, so it must
    // not live across cycles the way `acc` and `client` do.
    void runCycle(
        io_context& ioc,
        capy::executor_ref ex,
        tcp_acceptor& acc,
        tcp_socket& client,
        endpoint ep)
    {
        tcp_socket server(ioc);

        bool accepted = false, read_ok = false;
        bool connected = false, write_ok = false;
        char in = 0;
        char const out = 'x';

        // Server side: accept into the reused socket, then echo the
        // one byte the client sends back to it.
        capy::run_async(ex)(
            [](tcp_acceptor& a, tcp_socket& s, char* b, bool& got_accept,
               bool& got_read) -> capy::task<> {
                auto [ec1]     = co_await a.accept(s);
                got_accept     = !ec1;
                auto [ec2, n2] = co_await s.read_some(
                    capy::mutable_buffer(b, 1));
                got_read = !ec2 && n2 == 1;
            }(acc, server, &in, accepted, read_ok));

        // Client side: connect, then send the one byte.
        capy::run_async(ex)(
            [](tcp_socket& s, endpoint ep_, char const* b,
               bool& got_connect, bool& got_write) -> capy::task<> {
                auto [ec1]     = co_await s.connect(ep_);
                got_connect    = !ec1;
                auto [ec2, n2] = co_await s.write_some(
                    capy::const_buffer(b, 1));
                got_write = !ec2 && n2 == 1;
            }(client, ep, &out, connected, write_ok));

        ioc.run();
        ioc.restart();

        BOOST_TEST(accepted);
        BOOST_TEST(connected);
        BOOST_TEST(write_ok);
        BOOST_TEST(read_ok);
        BOOST_TEST_EQ(in, out);

        client.close();

        // server destructs here -- recycles its impl for the next
        // cycle's construct() instead of freeing it.
    }

    void testAcceptEchoCloseIsZeroAlloc()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        acc.set_option(socket_option::reuse_address(true));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        auto port = acc.local_endpoint().port();
        endpoint ep(ipv4_address::loopback(), port);

        // client is pre-created and outlives every cycle: only the
        // accept side's socket construct()/destroy() is under the
        // gate here, matching the brief's accept(tcp_socket&) idiom.
        tcp_socket client(ioc);

        for (int i = 0; i < warmup_cycles; ++i)
            runCycle(ioc, ex, acc, client, ep);

        alloc_count.store(0, std::memory_order_relaxed);
        alloc_armed.store(true, std::memory_order_relaxed);
        for (int i = 0; i < measured_cycles; ++i)
            runCycle(ioc, ex, acc, client, ep);
        alloc_armed.store(false, std::memory_order_relaxed);

        // epoll: the reactor dispatch path (accept, connect, read,
        // write, close) and the recycled server-socket impl are both
        // allocation-free once warmed -- this is the task's primary
        // claim.
        //
        // select: registered_descs_ (select_scheduler.hpp) is a
        // std::map keyed by fd; register_descriptor()/
        // deregister_descriptor() insert/erase a node for each of the
        // 2 fds this cycle creates (the accepted server fd, the
        // reconnected client fd) every cycle, regardless of impl
        // recycling. Documented out of scope for this task (design
        // doc section 1); 2 allocations/cycle pins the known cost.
        //
        // uring: the multishot acceptor heap-allocates a fresh
        // uring_accept_op plus a waiter_node per accepted connection
        // (uring_multishot_acceptor.hpp) -- also out of scope here;
        // 2 allocations/cycle pins that known cost.
        //
        // Either way, pinning the exact number (0, or the documented
        // non-zero count) means a regression or an improvement both
        // trip this assert instead of silently drifting.
#if BOOST_COROSIO_HAS_EPOLL
        if constexpr (std::is_same_v<std::decay_t<decltype(Backend)>, epoll_t>)
            BOOST_TEST_EQ(alloc_count.load(std::memory_order_relaxed), 0LL);
        else
#endif
            BOOST_TEST_EQ(
                alloc_count.load(std::memory_order_relaxed),
                static_cast<long long>(2 * measured_cycles));
    }

    // Reuse identity at the tcp_socket level: once the pool has one
    // spare impl, a construct()/destroy()/construct() sequence pops
    // and recycles that same impl both times instead of allocating.
    void testReuseIdentityIsZeroAlloc()
    {
        io_context ioc(Backend);

        {
            tcp_socket warm(ioc); // first-ever construct: pool is cold
        } // destroy() recycles it

        alloc_count.store(0, std::memory_order_relaxed);
        alloc_armed.store(true, std::memory_order_relaxed);
        {
            tcp_socket s1(ioc); // pop from free_
        } // destroy() -> recycle
        {
            tcp_socket s2(ioc); // pop the same recycled impl again
        }
        alloc_armed.store(false, std::memory_order_relaxed);

        BOOST_TEST_EQ(alloc_count.load(std::memory_order_relaxed), 0LL);
    }

    // Timer churn: the thread-local single-slot tier plus capy's
    // frame-embedded waiter should make a delay() await literally
    // zero-alloc on every backend, not just the two the socket path
    // tolerates a known gap on.
    void testDelayChurnIsZeroAlloc()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        auto one_delay = [&] {
            bool done = false;
            capy::run_async(ex)([](bool& d) -> capy::task<> {
                auto [ec] = co_await delay(std::chrono::milliseconds(0));
                d         = !ec;
            }(done));
            ioc.run();
            ioc.restart();
            BOOST_TEST(done);
        };

        // Warms both the timer_service pool and capy's coroutine
        // frame pool -- the latter's own first-use allocation would
        // otherwise show up as noise in the counted window below.
        for (int i = 0; i < warmup_cycles; ++i)
            one_delay();

        alloc_count.store(0, std::memory_order_relaxed);
        alloc_armed.store(true, std::memory_order_relaxed);
        for (int i = 0; i < measured_cycles; ++i)
            one_delay();
        alloc_armed.store(false, std::memory_order_relaxed);

        BOOST_TEST_EQ(alloc_count.load(std::memory_order_relaxed), 0LL);
    }

    void run()
    {
        testAcceptEchoCloseIsZeroAlloc();
        testReuseIdentityIsZeroAlloc();
        testDelayChurnIsZeroAlloc();
    }
};

COROSIO_TEST_EPOLL_(zero_alloc_test, "boost.corosio.zero_alloc")
COROSIO_TEST_SELECT_(zero_alloc_test, "boost.corosio.zero_alloc")
COROSIO_TEST_URING_(zero_alloc_test, "boost.corosio.zero_alloc")

} // namespace boost::corosio

#endif // !COROSIO_TEST_HAS_ASAN && !COROSIO_TEST_HAS_TSAN
