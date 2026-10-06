//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/native/detail/kqueue/kqueue_scheduler.hpp>

#include <sys/event.h>
#include <unistd.h>

#include "test_suite.hpp"

namespace boost::corosio {

struct kqueue_lazy_write_registration_test
{
    static bool has_filter(int kq, int fd, short filter)
    {
        // EV_DELETE of a filter that is not registered fails with ENOENT.
        // The call goes through kqueue_call: a kqueue accepts one flavor
        // of kevent, and Darwin's is kevent64.
        detail::kqueue_event ch;
        detail::kqueue_set(ch, fd, filter, EV_DELETE, 0, nullptr);
        return detail::kqueue_call(kq, &ch, 1, nullptr, 0, nullptr) == 0;
    }

    void testAdoptRegistersReadOnly()
    {
        int kq = ::kqueue();
        int fds[2];
        BOOST_TEST_EQ(::pipe(fds), 0);
        BOOST_TEST_EQ(
            detail::kqueue_add_filter(kq, fds[0], EVFILT_READ, nullptr), 0);
        BOOST_TEST(has_filter(kq, fds[0], EVFILT_READ));
        BOOST_TEST(!has_filter(kq, fds[0], EVFILT_WRITE));
        ::close(fds[0]);
        ::close(fds[1]);
        ::close(kq);
    }

    void testRefusedFilterReportsErrno()
    {
        // A kqueue descriptor has a read filter only.
        int kq    = ::kqueue();
        int inner = ::kqueue();
        BOOST_TEST_EQ(
            detail::kqueue_add_filter(kq, inner, EVFILT_WRITE, nullptr),
            EINVAL);
        BOOST_TEST(!has_filter(kq, inner, EVFILT_WRITE));
        ::close(inner);
        ::close(kq);
    }

    void run()
    {
        testAdoptRegistersReadOnly();
        testRefusedFilterReportsErrno();
    }
};

TEST_SUITE(
    kqueue_lazy_write_registration_test,
    "boost.corosio.native.kqueue.lazy_write_registration");

} // namespace boost::corosio

#endif
