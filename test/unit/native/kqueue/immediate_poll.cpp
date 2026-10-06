//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// On Darwin a kevent() given a zero timespec still costs ~12us when
// nothing is ready; only kevent64() with KEVENT_FLAG_IMMEDIATE returns
// at once. The scheduler makes every kqueue call through kqueue_call,
// so a zero timeout there must take the fast path, or every poll() and
// every run-loop pass that polls with work queued pays the slow one.
//
// The test times kqueue_call alone, not a whole poll(): sanitizers
// slow the scheduler's own code by microseconds, but add little to a
// single syscall. It takes the median, because the slow path is ~12us
// typically but occasionally under 4us, while the fast one is ~0.1us.

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE && defined(__APPLE__)

#include <boost/corosio/native/detail/kqueue/kqueue_scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>

#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>

#include "test_suite.hpp"

namespace boost::corosio {

struct kqueue_immediate_poll_test
{
    void testEmptyPollIsImmediate()
    {
        int kq = ::kqueue();
        int fds[2];
        BOOST_TEST_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

        // A watched socket with nothing to read: the poll finds no event.
        BOOST_TEST_EQ(
            detail::kqueue_add_filter(kq, fds[0], EVFILT_READ, nullptr), 0);

        detail::kqueue_event events[16];
        struct timespec const zero = {0, 0};

        constexpr int samples = 501;
        std::int64_t ns[samples];
        for (auto& d : ns)
        {
            auto const t0 = std::chrono::steady_clock::now();
            int n = detail::kqueue_call(kq, nullptr, 0, events, 16, &zero);
            d     = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count();
            BOOST_TEST_EQ(n, 0);
        }
        std::nth_element(ns, ns + samples / 2, ns + samples);
        BOOST_TEST_LT(ns[samples / 2], 2000);

        ::close(fds[0]);
        ::close(fds[1]);
        ::close(kq);
    }

    void run()
    {
        testEmptyPollIsImmediate();
    }
};

TEST_SUITE(
    kqueue_immediate_poll_test, "boost.corosio.native.kqueue.immediate_poll");

} // namespace boost::corosio

#endif
