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
#include <boost/corosio/random_access_file.hpp>
#include <boost/corosio/tcp_server.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/delay.hpp>
#include <boost/corosio/detail/platform.hpp>
#if BOOST_COROSIO_POSIX
#include <boost/corosio/posix_stream_descriptor.hpp>
#include <unistd.h>
#endif

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <memory>
#include <stop_token>
#include <tuple>
#include <vector>
#include <type_traits>

#include "temp_path.hpp"
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

        // Client side: connect, then send the one byte. A failed connect
        // leaves nothing for the accept to receive.
        capy::run_async(ex)(
            [](tcp_socket& s, tcp_acceptor& a, endpoint ep_, char const* b,
               bool& got_connect, bool& got_write) -> capy::task<> {
                auto [ec1]  = co_await s.connect(ep_);
                got_connect = !ec1;
                if (ec1)
                {
                    a.cancel();
                    co_return;
                }
                auto [ec2, n2] = co_await s.write_some(
                    capy::const_buffer(b, 1));
                got_write = !ec2 && n2 == 1;
            }(client, acc, ep, &out, connected, write_ok));

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
        // gate here.
        tcp_socket client(ioc);

        for (int i = 0; i < warmup_cycles; ++i)
            runCycle(ioc, ex, acc, client, ep);

        alloc_count.store(0, std::memory_order_relaxed);
        alloc_armed.store(true, std::memory_order_relaxed);
        for (int i = 0; i < measured_cycles; ++i)
            runCycle(ioc, ex, acc, client, ep);
        alloc_armed.store(false, std::memory_order_relaxed);

        // epoll/kqueue: the reactor dispatch path (accept, connect,
        // read, write, close) and the recycled server-socket impl are
        // both allocation-free once warmed.
        //
        // select: registered_descs_ (select_scheduler.hpp) is a
        // std::map keyed by fd; register_descriptor()/
        // deregister_descriptor() insert/erase a node for each of the
        // 2 fds this cycle creates (the accepted server fd, the
        // reconnected client fd) every cycle, regardless of impl
        // recycling; 2 allocations/cycle pins that known cost.
        //
        // uring: the multishot acceptor's accept nodes and ready-fd
        // nodes recycle through per-acceptor free lists
        // (uring_multishot_acceptor.hpp), so warmed accept churn is
        // allocation-free like epoll.
        //
        // Either way, pinning the exact number (0, or the documented
        // non-zero count) means a regression or an improvement both
        // trip this assert instead of silently drifting.
#if BOOST_COROSIO_HAS_SELECT
        if constexpr (
            std::is_same_v<std::decay_t<decltype(Backend)>, select_t>)
            BOOST_TEST_EQ(
                alloc_count.load(std::memory_order_relaxed),
                static_cast<long long>(2 * measured_cycles));
        else
#endif
            BOOST_TEST_EQ(alloc_count.load(std::memory_order_relaxed), 0LL);
    }

    // One acceptor reopened and re-listened each cycle. On uring each
    // listen arms a multishot accept and each close cancels it; the
    // ended armings recycle through the acceptor once their terminal
    // completions are reaped.
    void listenCycle(io_context& ioc, capy::executor_ref ex, tcp_acceptor& acc)
    {
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        acc.close();
        for (int i = 0; i < 4; ++i)
        {
            capy::run_async(ex)([]() -> capy::task<> { co_return; }());
            ioc.restart();
            ioc.poll();
        }
    }

    void testListenChurnIsZeroAlloc()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        tcp_acceptor acc(ioc);

        for (int i = 0; i < warmup_cycles; ++i)
            listenCycle(ioc, ex, acc);

        alloc_count.store(0, std::memory_order_relaxed);
        alloc_armed.store(true, std::memory_order_relaxed);
        for (int i = 0; i < measured_cycles; ++i)
            listenCycle(ioc, ex, acc);
        alloc_armed.store(false, std::memory_order_relaxed);

        // select pays its known map node for the one fd each cycle
        // registers; see testAcceptEchoCloseIsZeroAlloc.
#if BOOST_COROSIO_HAS_SELECT
        if constexpr (
            std::is_same_v<std::decay_t<decltype(Backend)>, select_t>)
            BOOST_TEST_EQ(
                alloc_count.load(std::memory_order_relaxed),
                static_cast<long long>(measured_cycles));
        else
#endif
            BOOST_TEST_EQ(alloc_count.load(std::memory_order_relaxed), 0LL);
    }

    // Reuse identity at the tcp_socket level: once the pool has one
    // spare impl, a construct()/destroy()/construct() sequence pops
    // and recycles that same impl both times instead of allocating.
#if BOOST_COROSIO_POSIX
    // A descriptor adopted, read through once, and destroyed: the
    // recycled impl and its embedded ops make the cycle allocation-free.
    void descriptorCycle(io_context& ioc, capy::executor_ref ex, int (&fds)[2])
    {
        posix_stream_descriptor d(ioc);
        BOOST_TEST(!d.assign(::dup(fds[0])));
        BOOST_TEST_EQ(::write(fds[1], "x", 1), 1);
        bool got = false;
        char b   = 0;
        capy::run_async(ex)(
            [](posix_stream_descriptor& d, char* b, bool& got) -> capy::task<> {
                auto [ec, n] = co_await d.read_some(capy::mutable_buffer(b, 1));
                got          = !ec && n == 1;
            }(d, &b, got));
        ioc.run();
        ioc.restart();
        BOOST_TEST(got);
    }

    void testDescriptorChurnIsZeroAlloc()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        int fds[2];
        BOOST_TEST_EQ(::pipe(fds), 0);

        for (int i = 0; i < warmup_cycles; ++i)
            descriptorCycle(ioc, ex, fds);

        alloc_count.store(0, std::memory_order_relaxed);
        alloc_armed.store(true, std::memory_order_relaxed);
        for (int i = 0; i < measured_cycles; ++i)
            descriptorCycle(ioc, ex, fds);
        alloc_armed.store(false, std::memory_order_relaxed);

        // select pays its known map node for the one fd each cycle
        // registers; see testAcceptEchoCloseIsZeroAlloc.
#if BOOST_COROSIO_HAS_SELECT
        if constexpr (
            std::is_same_v<std::decay_t<decltype(Backend)>, select_t>)
            BOOST_TEST_EQ(
                alloc_count.load(std::memory_order_relaxed),
                static_cast<long long>(measured_cycles));
        else
#endif
            BOOST_TEST_EQ(alloc_count.load(std::memory_order_relaxed), 0LL);
        ::close(fds[0]);
        ::close(fds[1]);
    }
#endif

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
    // zero-alloc on every backend, including select, whose socket
    // path has a known per-fd cost. The delay must be non-zero: a
    // zero delay completes inline without touching the timer service.
    void testDelayChurnIsZeroAlloc()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        auto one_delay = [&] {
            bool done = false;
            capy::run_async(ex)([](bool& d) -> capy::task<> {
                auto [ec] = co_await delay(std::chrono::milliseconds(1));
                d         = !ec;
            }(done));
            ioc.run();
            ioc.restart();
            BOOST_TEST(done);
        };

        // A wait that finds its timer already expired never touches
        // the expiry heap, so a delay alone may never grow it during
        // warmup. One cancelled long wait grows it deterministically.
        {
            std::stop_source stop;
            capy::run_async(ex, stop.get_token())([]() -> capy::task<> {
                std::ignore = co_await delay(std::chrono::hours(1));
            }());
            std::ignore = ioc.poll();
            stop.request_stop();
            ioc.run();
            ioc.restart();
        }

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

    // Random-access file churn: each read_some_at/write_some_at used
    // to heap-allocate its op; both now recycle through the file's
    // free lists, so a warmed write+read round trip is zero-alloc on
    // every backend (posix thread-pool path and native uring path).
    void testRafChurnIsZeroAlloc()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        test::temp_file tmp("zero_alloc_raf_", "warm");
        random_access_file f(ioc);
        BOOST_TEST(!f.open(tmp.path, file_base::read_write));

        char buf[4] = {};
        auto one_rw = [&] {
            bool wrote = false, read = false;
            capy::run_async(ex)(
                [](random_access_file& file, char* b, bool& w,
                   bool& r) -> capy::task<> {
                    auto [ec1, n1] = co_await file.write_some_at(
                        0, capy::const_buffer("abcd", 4));
                    w              = !ec1 && n1 == 4;
                    auto [ec2, n2] = co_await file.read_some_at(
                        0, capy::mutable_buffer(b, 4));
                    r = !ec2 && n2 == 4;
                }(f, buf, wrote, read));
            ioc.run();
            ioc.restart();
            BOOST_TEST(wrote);
            BOOST_TEST(read);
        };

        for (int i = 0; i < warmup_cycles; ++i)
            one_rw();

        alloc_count.store(0, std::memory_order_relaxed);
        alloc_armed.store(true, std::memory_order_relaxed);
        for (int i = 0; i < measured_cycles; ++i)
            one_rw();
        alloc_armed.store(false, std::memory_order_relaxed);

        BOOST_TEST_EQ(alloc_count.load(std::memory_order_relaxed), 0LL);
    }

    // tcp_server connection churn: launching a worker used to cost a
    // fresh stop_source state, a globally allocated launch_wrapper
    // frame, and a heap wrapper op for the bare-handle post. All
    // three now recycle (reused stop state, frame_alloc_mixin,
    // frame-embedded continuation), so a warmed connect/echo/close
    // round trip through the server allocates nothing. Select keeps
    // its documented 2/cycle registered_descs_ cost.
    void testServerChurnIsZeroAlloc()
    {
        struct echo_worker : tcp_server::worker_base
        {
            io_context& ctx;
            tcp_socket sock;
            explicit echo_worker(io_context& c) : ctx(c), sock(c) {}
            tcp_socket& socket() override
            {
                return sock;
            }
            void run(tcp_server::launcher launch) override
            {
                launch(
                    ctx.get_executor(),
                    [](tcp_socket* s) -> capy::task<>
                    {
                        char b[8];
                        auto [ec, n] = co_await s->read_some(
                            capy::mutable_buffer(b, sizeof(b)));
                        if (!ec)
                            std::ignore = co_await s->write_some(
                                capy::const_buffer(b, n));
                        s->close();
                    }(&sock));
            }
        };

        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_server srv(ioc, ex);
        {
            std::vector<std::unique_ptr<tcp_server::worker_base>> v;
            for (int i = 0; i < 2; ++i)
                v.push_back(std::make_unique<echo_worker>(ioc));
            srv.set_workers(std::move(v));
        }
        BOOST_TEST(!srv.bind(endpoint(ipv4_address::loopback(), 0)));
        auto port = srv.local_endpoint().port();
        endpoint ep(ipv4_address::loopback(), port);
        srv.start();

        auto one_conn = [&] {
            bool echoed = false;
            capy::run_async(ex)(
                [](io_context& c, endpoint target, bool& ok) -> capy::task<>
                {
                    tcp_socket s(c);
                    auto [ec1] = co_await s.connect(target);
                    if (ec1)
                        co_return;
                    char const out = 'z';
                    auto [ec2, n2] =
                        co_await s.write_some(capy::const_buffer(&out, 1));
                    if (ec2 || n2 != 1)
                        co_return;
                    char in        = 0;
                    auto [ec3, n3] =
                        co_await s.read_some(capy::mutable_buffer(&in, 1));
                    ok = !ec3 && n3 == 1 && in == out;
                }(ioc, ep, echoed));
            // Pump one event at a time: the server's pending accept
            // keeps the context busy forever, so a plain run() (or a
            // full run_for window) would never return promptly.
            while (!echoed && ioc.run_one_for(std::chrono::seconds(5)) != 0)
            {
            }
            ioc.restart();
            BOOST_TEST(echoed);
        };

        for (int i = 0; i < warmup_cycles; ++i)
            one_conn();

        alloc_count.store(0, std::memory_order_relaxed);
        alloc_armed.store(true, std::memory_order_relaxed);
        for (int i = 0; i < measured_cycles; ++i)
            one_conn();
        alloc_armed.store(false, std::memory_order_relaxed);

#if BOOST_COROSIO_HAS_SELECT
        if constexpr (
            std::is_same_v<std::decay_t<decltype(Backend)>, select_t>)
            BOOST_TEST_EQ(
                alloc_count.load(std::memory_order_relaxed),
                static_cast<long long>(2 * measured_cycles));
        else
#endif
            BOOST_TEST_EQ(alloc_count.load(std::memory_order_relaxed), 0LL);

        srv.stop();
        ioc.run();
    }

    void run()
    {
        if (!alloc_counter_is_live("boost.corosio.zero_alloc"))
            return;
        testAcceptEchoCloseIsZeroAlloc();
        testReuseIdentityIsZeroAlloc();
#if BOOST_COROSIO_POSIX
        testDescriptorChurnIsZeroAlloc();
#endif
        testListenChurnIsZeroAlloc();
        testDelayChurnIsZeroAlloc();
        testRafChurnIsZeroAlloc();
        testServerChurnIsZeroAlloc();
    }
};

COROSIO_TEST_EPOLL_(zero_alloc_test, "boost.corosio.zero_alloc")
COROSIO_TEST_KQUEUE_(zero_alloc_test, "boost.corosio.zero_alloc")
COROSIO_TEST_SELECT_(zero_alloc_test, "boost.corosio.zero_alloc")
COROSIO_TEST_URING_(zero_alloc_test, "boost.corosio.zero_alloc")

} // namespace boost::corosio

#endif // !COROSIO_TEST_HAS_ASAN && !COROSIO_TEST_HAS_TSAN
