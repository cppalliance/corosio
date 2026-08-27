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
#include <optional>
#include <system_error>

#if BOOST_COROSIO_POSIX

namespace boost::corosio::test::fault {

/* Creating the signal self-pipe, in a process where it does not exist.

   The pipe is a process-wide singleton latched on success, so every
   fault below is reachable exactly once per process and only while it
   is still unopened. That is what makes this a suite of its own rather
   than a test: CTest runs one process per suite, and b2 one executable
   per source. The suite name sorts after every other fault suite, so a
   single-process run of the whole executable still meets the tests in
   an order where the pipe is still closed when they need it to be.

   Within the suite the order is load-bearing too, and run() spells it
   out: the creation faults first, then the one test that lets creation
   succeed, then the registration faults that need a pipe to register.
*/
struct signal_pipe_faults
{
    void testPipeCreateFails()
    {
        {
            io_context ioc(one_backend);
            signal_set ss(ioc);
            int const before = open_fds();
            fault_scope f(sys::pipe, EMFILE);
            auto ec = ss.add(SIGUSR2);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::too_many_files_open);
            BOOST_TEST_EQ(open_fds(), before);
        }
        // Three fcntl calls configure each end, read end first: 1-3
        // fail the read end, 4-6 the write end. Either way both ends
        // are closed before the error is reported.
        for(unsigned nth : {1u, 2u, 3u, 4u, 5u, 6u})
        {
            io_context ioc(one_backend);
            signal_set ss(ioc);
            int const before = open_fds();
            fault_scope f(sys::fcntl, EINVAL, nth);
            auto ec = ss.add(SIGUSR2);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::invalid_argument);
            BOOST_TEST_EQ(open_fds(), before);
        }
    }

    /* The first test that lets the pipe be created, and it creates it
       above FD_SETSIZE on purpose: the select scheduler watches the
       read end through an fd_set, so a number it cannot represent is
       refused there rather than in the pipe's own setup. Every later
       test in the process inherits that high-numbered pipe, which the
       other backends have no trouble with.
    */
    void testSelectReaderRejectsHighFd()
    {
#if BOOST_COROSIO_HAS_SELECT
        io_context ioc(select);
        signal_set ss(ioc);
        fd_wall wall;
        if(!wall.ok())
        {
            skip_no_high_fd("testSelectReaderRejectsHighFd");
            return;
        }
        auto ec = ss.add(SIGUSR2);
        BOOST_TEST(ec == std::errc::too_many_files_open);
#endif
    }

    void testReaderRegisterFails()
    {
#if BOOST_COROSIO_HAS_EPOLL
        constexpr sys register_call = sys::epoll_ctl;
#elif BOOST_COROSIO_HAS_KQUEUE
        constexpr sys register_call = sys::kevent;
#else
        return;
#endif
#if BOOST_COROSIO_HAS_EPOLL || BOOST_COROSIO_HAS_KQUEUE
        io_context ioc(one_backend);
        signal_set ss(ioc);
        std::error_code ec;
        {
            fault_scope f(register_call, ENOMEM);
            ec = ss.add(SIGUSR2);
            BOOST_TEST(f.fired());
        }
        BOOST_TEST(ec == std::errc::not_enough_memory);
        // Success-latched, so a failed registration is retried by the
        // next add() rather than lost.
        BOOST_TEST(!ss.add(SIGUSR2));
        BOOST_TEST(!ss.clear());
#endif
    }

#if BOOST_COROSIO_HAS_IO_URING
    void testUringReaderSubmitFails()
    {
        io_context ioc(io_uring);
        signal_set ss(ioc);
        std::error_code ec;
        {
            // The wakeup eventfd's submit is spent building the ring,
            // before this arm, so the reader's is the first it sees.
            fault_scope f(sys::io_uring_submit, EBADF);
            ec = ss.add(SIGUSR2);
            BOOST_TEST(f.fired());
        }
        BOOST_TEST(ec == std::errc::bad_file_descriptor);
        BOOST_TEST(!ss.add(SIGUSR2));
        BOOST_TEST(!ss.clear());
    }

    /* A ring clamped to one SQE spends it on the wakeup poll, leaving
       none for the signal reader's multishot poll. The submit that
       follows the failed acquisition succeeds because it has nothing
       left to submit, which is not the same as a reader watching the
       pipe, so the arm has to report rather than assume.
    */
    void testUringReaderSqFull()
    {
        // The clamp sizes the ring as it is created, which happens
        // while the context is built, so it is armed before that.
        std::optional<fault_scope> f;
        f.emplace(sys::uring_sqe_full, 0);
        io_context ioc(io_uring);
        signal_set ss(ioc);
        std::error_code const ec = ss.add(SIGUSR2);
        BOOST_TEST(f->fired());
        f.reset();
        BOOST_TEST(ec == std::errc::resource_unavailable_try_again);
        // With the SQ flushable again the next add() arms the reader.
        BOOST_TEST(!ss.add(SIGUSR2));
        BOOST_TEST(!ss.clear());
    }
#endif

    void run()
    {
        if(skip_under_valgrind())
            return;
        testPipeCreateFails();
        testSelectReaderRejectsHighFd();
        testReaderRegisterFails();
#if BOOST_COROSIO_HAS_IO_URING
        testUringReaderSubmitFails();
        testUringReaderSqFull();
#endif
    }
};

TEST_SUITE(signal_pipe_faults, "boost.corosio.fault.signal_pipe");

} // boost::corosio::test::fault

#endif
