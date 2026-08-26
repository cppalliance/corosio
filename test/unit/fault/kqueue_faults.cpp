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
#include "test_utils.hpp"

#include <boost/corosio/delay.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/signal_set.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <system_error>
#include <tuple>

#if BOOST_COROSIO_HAS_KQUEUE

namespace boost::corosio::test::fault {

namespace {

endpoint loopback()
{
    return endpoint(ipv4_address::loopback(), 0);
}

} // namespace

/* kqueue-specific fault coverage.

   Every registration, interruption and wait on this backend is one
   symbol, kevent(), so an arm's `nth` has to be read against the
   syscall order of the operation under test rather than against a
   symbol that only the run loop uses. The scheduler issues exactly one
   kevent() while constructing (the EVFILT_USER registration), so a
   scope armed after the io_context exists starts counting at the next
   one.
*/
struct kqueue_faults
{
    void testConstructorFails()
    {
        auto expect_throw = [](sys s, unsigned nth, int err, std::errc code)
        {
            int before = open_fds();
            fault_scope f(s, err, nth);
            expect_system_error([&]{ io_context ioc(kqueue); }, code);
            BOOST_TEST(f.fired());
            // Every constructor failure past kqueue() closes the queue
            // before throwing (the kqueue_scheduler constructor).
            BOOST_TEST_EQ(open_fds(), before);
        };
        expect_throw(sys::kqueue, 1, EMFILE,
            std::errc::too_many_files_open);
        // The only fcntl the scheduler makes is FD_CLOEXEC on the queue.
        expect_throw(sys::fcntl, 1, EINVAL,
            std::errc::invalid_argument);
        // 1 is the EVFILT_USER registration used to interrupt the wait.
        expect_throw(sys::kevent, 1, ENOMEM,
            std::errc::not_enough_memory);
    }

    void testOpenFails()
    {
        io_context ioc(kqueue);
        {
            tcp_socket s(ioc);
            fault_scope f(sys::socket, EMFILE);
            auto ec = s.open(tcp::v4());
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::too_many_files_open);
            BOOST_TEST(!s.is_open());
        }
        // F_GETFL, F_SETFL(O_NONBLOCK) and F_SETFD(FD_CLOEXEC), in that
        // order (kqueue_traits::set_fd_options).
        for(unsigned nth : {1u, 2u, 3u})
        {
            int before = open_fds();
            tcp_socket s(ioc);
            fault_scope f(sys::fcntl, EINVAL, nth);
            auto ec = s.open(tcp::v4());
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::invalid_argument);
            BOOST_TEST(!s.is_open());
            BOOST_TEST_EQ(open_fds(), before);
        }
        {
            // SO_NOSIGPIPE is the one setsockopt an AF_INET open
            // makes, and it is fatal: this backend's write policy
            // spells its writes writev() (write_policy::write) and
            // write() (write_policy::write_one), neither of which carries
            // per-call SIGPIPE suppression, so the socket-level flag
            // is the only guard.
            int before = open_fds();
            tcp_socket s(ioc);
            fault_scope f(sys::setsockopt, ENOPROTOOPT);
            auto ec = s.open(tcp::v4());
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::no_protocol_option);
            BOOST_TEST(!s.is_open());
            BOOST_TEST_EQ(open_fds(), before);
        }
        {
            // The socket already exists when registration fails, so the
            // failure path owns closing it.
            int before = open_fds();
            tcp_socket s(ioc);
            fault_scope f(sys::kevent, ENOMEM);
            auto ec = s.open(tcp::v4());
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::not_enough_memory);
            BOOST_TEST(!s.is_open());
            BOOST_TEST_EQ(open_fds(), before);
        }
    }

    void testAssignRegisterFails()
    {
        io_context ioc(kqueue);
        int before = open_fds();
        auto h = make_native_socket(AF_INET, SOCK_STREAM);
        make_native_adoptable(h);
        {
            // assign rolls its own registration failure back rather
            // than closing anything: fd_ and registered_events return
            // to their closed values and the caller keeps the
            // descriptor it passed in
            // (reactor_basic_socket::init_and_register).
            tcp_socket s(ioc);
            int held = open_fds();
            fault_scope f(sys::kevent, ENOMEM);
            auto ec = s.assign(h);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::not_enough_memory);
            BOOST_TEST(!s.is_open());
            BOOST_TEST_EQ(open_fds(), held);
            BOOST_TEST(native_socket_valid(h));
        }
        close_native_socket(h);
        BOOST_TEST_EQ(open_fds(), before);
    }

    void testAcceptorRegisterFails()
    {
        io_context ioc(kqueue);
        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(loopback()));
        // An acceptor reaches register_descriptor for the first time at
        // listen (reactor_acceptor::do_listen); open only creates and
        // configures the fd.
        fault_scope f(sys::kevent, ENOMEM);
        BOOST_TEST(acc.listen() == std::errc::not_enough_memory);
        BOOST_TEST(f.fired());
        // Not latched: registered_events stayed 0, so a second listen
        // re-registers (kqueue_scheduler::register_descriptor).
        BOOST_TEST(!acc.listen());
    }

    void testAcceptFails()
    {
        io_context ioc(kqueue);
        tcp_acceptor acc(ioc, loopback());
        std::error_code aec, aec2;
        int leaked = 0;
        auto body = [&]() -> capy::task<>
        {
            tcp_socket client(ioc), server(ioc);
            {
                auto [ec] = co_await client.connect(acc.local_endpoint());
                BOOST_TEST(!ec);
            }
            {
                // EINTR is retried inside accept_policy, then the real
                // accept succeeds (accept_policy::do_accept).
                fault_scope f(sys::accept, EINTR);
                auto [ec] = co_await acc.accept(server);
                BOOST_TEST(f.fired());
                BOOST_TEST(!ec);
            }
            server.close();
            client.close();
            tcp_socket client2(ioc);
            {
                auto [ec] = co_await client2.connect(acc.local_endpoint());
                BOOST_TEST(!ec);
            }
            {
                // The faulted accept never ran, so the queued connection
                // survives for the retry below.
                fault_scope f(sys::accept, ECONNABORTED);
                auto [ec] = co_await acc.accept(server);
                aec = ec;
                BOOST_TEST(f.fired());
                BOOST_TEST(!server.is_open());
            }
            {
                auto [ec] = co_await acc.accept(server);
                BOOST_TEST(!ec);
            }
            server.close();
            client2.close();
            {
                // The accepted fd fails to register: the impl is
                // destroyed, which closes it, and the error is reported
                // (reactor_acceptor_impl::accept).
                tcp_socket client3(ioc);
                auto [cec] = co_await client3.connect(acc.local_endpoint());
                BOOST_TEST(!cec);
                int before = open_fds();
                fault_scope f(sys::kevent, ENOMEM);
                auto [ec] = co_await acc.accept(server);
                aec2 = ec;
                leaked = open_fds() - before;
                BOOST_TEST(f.fired());
                BOOST_TEST(!server.is_open());
                client3.close();
            }
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST(aec == std::errc::connection_aborted);
        BOOST_TEST(aec2 == std::errc::not_enough_memory);
        BOOST_TEST_EQ(leaked, 0);
    }

    void testAcceptConfigureFails()
    {
        io_context ioc(kqueue);
        tcp_acceptor acc(ioc, loopback());
        std::error_code fcntl_ecs[3], sockopt_ec;
        int fcntl_leaked[3] = {}, sockopt_leaked = 0;
        auto body = [&]() -> capy::task<>
        {
            // Each failure closes the descriptor accept() already
            // handed back, consuming the queued connection, so every
            // iteration needs a client of its own.
            for(unsigned nth = 1; nth <= 3; ++nth)
            {
                tcp_socket c(ioc), s(ioc);
                {
                    auto [ec] = co_await c.connect(acc.local_endpoint());
                    BOOST_TEST(!ec);
                }
                int before = open_fds();
                // Armed after the connect, so 1..3 are the F_GETFL,
                // F_SETFL and F_SETFD accept_policy makes on the
                // accepted fd (accept_policy::do_accept).
                fault_scope f(sys::fcntl, EINVAL, nth);
                auto [ec] = co_await acc.accept(s);
                fcntl_ecs[nth - 1] = ec;
                fcntl_leaked[nth - 1] = open_fds() - before;
                BOOST_TEST(f.fired());
                BOOST_TEST(!s.is_open());
                c.close();
            }
            {
                tcp_socket c(ioc), s(ioc);
                {
                    auto [ec] = co_await c.connect(acc.local_endpoint());
                    BOOST_TEST(!ec);
                }
                int before = open_fds();
                // SO_NOSIGPIPE on the accepted fd, fatal for the
                // same reason (accept_policy::do_accept).
                fault_scope f(sys::setsockopt, ENOPROTOOPT);
                auto [ec] = co_await acc.accept(s);
                sockopt_ec = ec;
                sockopt_leaked = open_fds() - before;
                BOOST_TEST(f.fired());
                BOOST_TEST(!s.is_open());
                c.close();
            }
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        for(unsigned i = 0; i < 3; ++i)
        {
            BOOST_TEST(fcntl_ecs[i] == std::errc::invalid_argument);
            BOOST_TEST_EQ(fcntl_leaked[i], 0);
        }
        BOOST_TEST(sockopt_ec == std::errc::no_protocol_option);
        BOOST_TEST_EQ(sockopt_leaked, 0);
    }

    void testRunLoopFaults()
    {
        // Nothing calls kevent between the constructor and the run
        // loop's first wait: posting the task does not interrupt a
        // reactor that is not running yet
        // (reactor_scheduler::wake_one_thread_and_unlock), so nth 1 is
        // that wait.
        {
            io_context ioc(kqueue);
            fault_scope f(sys::kevent, EINTR);
            bool done = false;
            auto body = [&]() -> capy::task<>
            {
                std::ignore = co_await corosio::delay(
                    std::chrono::milliseconds(1));
                done = true;
            };
            capy::run_async(ioc.get_executor())(body());
            ioc.run();
            BOOST_TEST(f.fired());
            BOOST_TEST(done);
        }
        {
            // Any other errno leaves run() through an exception
            // (kqueue_scheduler::run_task).
            io_context ioc(kqueue);
            fault_scope f(sys::kevent, EBADF);
            auto body = [&]() -> capy::task<>
            {
                std::ignore = co_await corosio::delay(
                    std::chrono::milliseconds(1));
            };
            capy::run_async(ioc.get_executor())(body());
            expect_system_error([&]{ ioc.run(); },
                std::errc::bad_file_descriptor);
            BOOST_TEST(f.fired());
        }
    }

    /* A NOTE_TRIGGER that never left has to be retried, not swallowed.

       `user_event_armed_` is set before the kevent, so it stands for a
       trigger queued on the kqueue. A kevent that failed queued none:
       leaving the flag set would coalesce every later interrupt into a
       wake that does not exist, and a wait only an interrupt could end
       would never end. The second arm is the observable — it can only
       fire if the second interrupt actually reached kevent.
    */
    void testInterruptTriggerFails()
    {
        io_context ioc(kqueue);
        {
            // stop() is the one path that reaches NOTE_TRIGGER without
            // a reactor thread (reactor_scheduler::stop), and it
            // interrupts only on the transition, so each stop below is
            // one kevent.
            fault_scope first(sys::kevent, EIO, 1);
            fault_scope second(sys::kevent, EIO, 2);
            ioc.stop();
            BOOST_TEST(first.fired());
            BOOST_TEST(!second.fired());
            ioc.restart();
            ioc.stop();
            BOOST_TEST(second.fired());
        }
        // And the loop the failed wakes were aimed at still runs: timed
        // work fires and a stop from inside ends run().
        ioc.restart();
        bool done = false;
        auto body = [&]() -> capy::task<>
        {
            std::ignore = co_await corosio::delay(
                std::chrono::milliseconds(1));
            done = true;
            ioc.stop();
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST(done);
    }

    void testSignalReaderRegisterFails()
    {
        in_child([]{
            io_context ioc(kqueue);
            signal_set ss(ioc);
            std::error_code ec;
            bool fired = false;
            {
                // The self-pipe's pipe() and six fcntl() calls come
                // first; the kevent is the reader registration
                // (posix_signal_service::add_signal).
                fault_scope f(sys::kevent, ENOMEM);
                ec = ss.add(SIGUSR2);
                fired = f.fired();
            }
            // Not latched: the next add retries the registration.
            return fired && ec == std::errc::not_enough_memory &&
                !ss.add(SIGUSR2) && !ss.clear();
        });
    }

    void run()
    {
        if(skip_under_valgrind())
            return;
        testConstructorFails();
        testOpenFails();
        testAssignRegisterFails();
        testAcceptorRegisterFails();
        testAcceptFails();
        testAcceptConfigureFails();
        testRunLoopFaults();
        testInterruptTriggerFails();
        testSignalReaderRegisterFails();
    }
};

TEST_SUITE(kqueue_faults, "boost.corosio.fault.kqueue");

} // boost::corosio::test::fault

#endif
