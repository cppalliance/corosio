//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "fault.hpp"
#include "fault_test_utils.hpp"
#include "context.hpp"
#include "test_suite.hpp"

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/signal_set.hpp>

#include <cerrno>
#include <csignal>
#include <system_error>

#if BOOST_COROSIO_POSIX

namespace boost::corosio::test::fault {

/* Installing and restoring a signal disposition.

   sigaction() is only reached on the transition at each end of a
   signal's global registration count, so each test here has to own
   that count for the signal it uses and hand it back before the next
   one runs. A suite of its own for the same reason the pipe faults
   have one: the disposition is process state, and a suite that shares
   a process with tests that register signals cannot say what the
   count was when it started.
*/
struct signal_sigaction_faults
{
    void testAddFails()
    {
        io_context ioc(one_backend);
        signal_set ss(ioc);
        {
            fault_scope f(sys::sigaction, EINVAL);
            auto ec = ss.add(SIGUSR2);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::invalid_argument);
        }
        // The half-built registration is destroyed rather than linked,
        // so the retry starts from an empty set and installs.
        BOOST_TEST(!ss.add(SIGUSR2));
        BOOST_TEST(!ss.clear());
    }

    void testRemoveFails()
    {
        io_context ioc(one_backend);
        signal_set ss(ioc);
        BOOST_TEST(!ss.add(SIGUSR2));
        {
            fault_scope f(sys::sigaction, EINVAL);
            BOOST_TEST(ss.remove(SIGUSR2) == std::errc::invalid_argument);
            BOOST_TEST(f.fired());
        }
        // A failed restore leaves the registration in place, so the
        // count and the disposition stay in step and the retry works.
        BOOST_TEST(!ss.remove(SIGUSR2));
        BOOST_TEST(!ss.clear());
    }

    void testClearFails()
    {
        io_context ioc(one_backend);
        signal_set ss(ioc);
        BOOST_TEST(!ss.add(SIGUSR2));
        {
            fault_scope f(sys::sigaction, EINVAL);
            BOOST_TEST(ss.clear() == std::errc::invalid_argument);
            BOOST_TEST(f.fired());
        }
        // clear() reports the first failure but still empties the set,
        // so there is nothing left for a retry to find.
        BOOST_TEST(!ss.clear());
    }

    void run()
    {
        if(skip_under_valgrind())
            return;
        testAddFails();
        testRemoveFails();
        testClearFails();
    }
};

TEST_SUITE(signal_sigaction_faults, "boost.corosio.fault.signal_sigaction");

} // boost::corosio::test::fault

#endif
