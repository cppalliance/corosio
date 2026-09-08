//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "test_suite.hpp"

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_URING

#include <boost/corosio/backend.hpp>
#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstdint>

// The coroutine tasks below take their state through by-value parameters
// (references to test-scope objects), never by lambda capture: a captured
// closure is a temporary destroyed at the end of the run_async statement,
// which the suspended coroutine would then read through a dangling `this`.

namespace boost::corosio {

// Exposes the io_uring scheduler's internal in-flight counter so the
// teardown-accounting test can assert it stays balanced.
struct uring_test_context : native_io_context<uring>
{
    std::int64_t inflight()
    {
        return static_cast<detail::uring_scheduler*>(sched_)->inflight();
    }
};

struct multishot_acceptor_test
{
    void testContextConstructs()
    {
        native_io_context<uring> ioc;
        BOOST_TEST(!ioc.stopped());
    }

    // Regression: drain_cqes_for (run from the multishot acceptor's
    // destructor) used to advance CQEs without decrementing
    // uring_inflight_, leaking the counts of the multishot accept SQE
    // and the cancel SQEs it submits. The counter gates the do_one ring
    // pump, so the leak accumulated across every acceptor teardown for the
    // lifetime of the io_context. After the fix the counter returns to
    // zero once teardown settles.
    void testDrainCqesBalancesInflight()
    {
        uring_test_context ctx;

        // No io_uring SQEs are counted before any op is submitted.
        BOOST_TEST_EQ(ctx.inflight(), 0);

        {
            tcp_acceptor acc(ctx);
            BOOST_TEST(!acc.open());
            acc.set_option(socket_option::reuse_address(true));
            BOOST_TEST(!acc.bind(endpoint(0)));
            // listen() calls start_multishot(), submitting a multishot
            // accept SQE (counted once).
            BOOST_TEST(!acc.listen());

            // Flush the SQE to the kernel so it is genuinely in flight.
            ctx.poll();
            BOOST_TEST(ctx.inflight() >= 1);
        }
        // acc destroyed: the impl destructor runs cancel-by-fd and
        // drain_cqes_for(multi_op_). Drain the CQEs those produce that the
        // destructor's bounded loop did not itself consume.
        for (int i = 0; i < 64 && ctx.inflight() != 0; ++i)
            ctx.poll();

        // Every submitted SQE (the multishot accept plus the teardown
        // cancels) is now accounted for. Before the fix this stayed > 0.
        BOOST_TEST_EQ(ctx.inflight(), 0);
    }

    // A connection the multishot SQE delivers with no accept parked is
    // buffered in ready_fds_; a later accept() consumes it on the immediate
    // path instead of parking a waiter. The connection is driven to
    // completion first (its accept CQE buffers server-side), then a
    // separate run() posts the accept that consumes the buffered fd.
    void testAcceptBufferedConnection()
    {
        native_io_context<uring> ioc;
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        acc.set_option(socket_option::reuse_address(true));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        endpoint local = acc.local_endpoint();

        tcp_socket client(ioc);
        bool connected = false;
        capy::run_async(ex)(
            [](tcp_socket& c, endpoint ep, bool& done) -> capy::task<> {
                auto [ec] = co_await c.connect(ep);
                done      = !ec;
            }(client, local, connected));
        ioc.run(); // connector completes; the server accept CQE buffers
        BOOST_TEST(connected);

        ioc.restart();
        bool accepted = false;
        capy::run_async(ex)(
            [](tcp_acceptor& a, bool& done) -> capy::task<> {
                auto [ec, peer] = co_await a.accept();
                done = !ec && peer.is_open();
            }(acc, accepted));
        ioc.run(); // accept consumes the buffered fd
        BOOST_TEST(accepted);

        client.close();
        acc.close();
    }

    // An accept parked before any connection arrives is matched by the
    // multishot delivery when the connection lands (the waiter-match path,
    // distinct from consuming a pre-buffered fd).
    void testAcceptParkedThenDelivered()
    {
        native_io_context<uring> ioc;
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        acc.set_option(socket_option::reuse_address(true));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        endpoint local = acc.local_endpoint();

        bool accepted = false;
        capy::run_async(ex)(
            [](tcp_acceptor& a, bool& done) -> capy::task<> {
                auto [ec, peer] = co_await a.accept();
                done = !ec && peer.is_open();
            }(acc, accepted));
        // Park the accept without a connection present.
        for (int i = 0; i < 16; ++i)
            ioc.poll();
        BOOST_TEST(!accepted);

        tcp_socket client(ioc);
        capy::run_async(ex)(
            [](tcp_socket& c, endpoint ep) -> capy::task<> {
                std::ignore = co_await c.connect(ep);
            }(client, local));
        for (int i = 0; i < 10000 && !accepted; ++i) // delivery matches waiter
            ioc.poll();
        BOOST_TEST(accepted);

        client.close();
        acc.close();
    }

    // Buffered connections never accepted must be closed when the acceptor
    // is destroyed (the ready_fds_ drain in the impl destructor).
    void testDestroyWithBufferedConnections()
    {
        native_io_context<uring> ioc;
        auto ex = ioc.get_executor();

        tcp_socket c1(ioc), c2(ioc);
        {
            tcp_acceptor acc(ioc);
            BOOST_TEST(!acc.open());
            acc.set_option(socket_option::reuse_address(true));
            BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
            BOOST_TEST(!acc.listen());
            endpoint local = acc.local_endpoint();

            int connected = 0;
            auto run_connect = [&](tcp_socket& c) {
                capy::run_async(ex)(
                    [](tcp_socket& s, endpoint ep, int& n) -> capy::task<> {
                        auto [ec] = co_await s.connect(ep);
                        if (!ec)
                            ++n;
                    }(c, local, connected));
            };
            run_connect(c1);
            run_connect(c2);
            ioc.run(); // both connect; both server accept CQEs buffer
            BOOST_TEST_EQ(connected, 2);
            // acc destroyed here holding two buffered fds -> ready_fds_ drain.
        }
        c1.close();
        c2.close();
    }

    void run()
    {
        testContextConstructs();
        testDrainCqesBalancesInflight();
        testAcceptBufferedConnection();
        testAcceptParkedThenDelivered();
        testDestroyWithBufferedConnections();
    }
};

TEST_SUITE(
    multishot_acceptor_test,
    "boost.corosio.native.uring.multishot_acceptor");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_URING
