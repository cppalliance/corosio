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

#if BOOST_COROSIO_HAS_IO_URING

#include <boost/corosio/backend.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>

#include <cstdint>

namespace boost::corosio {

/* io_uring-specific test placeholders.

   Most io_uring behaviors (multishot accept queueing, cancel-by-fd, op
   lifecycle) are exercised by the existing backend-templated test suites
   (tcp_acceptor.io_uring, tcp_socket.io_uring, cancel.io_uring, etc.).
   This file is the slot for io_uring-only tests when they're needed.

   Future additions when there's a specific behavior to pin:
   - SQ ring backpressure (>256 in-flight ops): current behavior surfaces
     EAGAIN synchronously per spec section 8; needs a deterministic
     fixture before testing.
   - Probe-and-fall-back: requires loading a seccomp filter at process
     start; deferred to test infrastructure work.
*/

// Exposes the io_uring scheduler's internal in-flight counter so the
// teardown-accounting test can assert it stays balanced.
struct io_uring_test_context : native_io_context<io_uring>
{
    std::int64_t inflight()
    {
        return static_cast<detail::io_uring_scheduler*>(sched_)->inflight();
    }
};

struct native_io_uring_specific_test
{
    void testTagAvailable()
    {
        // io_context constructed with the explicit io_uring tag should
        // work on any host where BOOST_COROSIO_HAS_IO_URING is 1.
        io_context ioc(io_uring);
        BOOST_TEST(!ioc.stopped());
    }

    // Regression: drain_cqes_for (run from the multishot acceptor's
    // destructor) used to advance CQEs without decrementing
    // io_uring_inflight_, leaking the counts of the multishot accept SQE
    // and the cancel SQEs it submits. The counter gates the do_one ring
    // pump, so the leak accumulated across every acceptor teardown for the
    // lifetime of the io_context. After the fix the counter returns to
    // zero once teardown settles.
    void testDrainCqesBalancesInflight()
    {
        io_uring_test_context ctx;

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

    void run()
    {
        testTagAvailable();
        testDrainCqesBalancesInflight();
    }
};

TEST_SUITE(
    native_io_uring_specific_test,
    "boost.corosio.native.io_uring_specific");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IO_URING
