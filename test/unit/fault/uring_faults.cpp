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
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/random_access_file.hpp>
#include <boost/corosio/signal_set.hpp>
#include <boost/corosio/stream_file.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <array>

#include <poll.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstring>
#include <optional>
#include <system_error>
#include <tuple>

#if BOOST_COROSIO_HAS_URING

#include <liburing.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::test::fault {

namespace {

endpoint
uring_loopback()
{
    return endpoint(ipv4_address::loopback(), 0);
}

} // namespace

struct uring_faults
{
    // The ring is created at the end of io_context construction, so a
    // ring the kernel refuses leaves no context behind to run: the
    // failure surfaces from the constructor, and what it opened on the
    // way in is released as the constructor unwinds.
    void testRingInitFails()
    {
        auto expect = [](sys s, int err, std::errc code) {
            int const before = open_fds();
            fault_scope f(s, err);
            expect_system_error([&] { io_context ioc(uring); }, code);
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(open_fds(), before);
        };
        expect(
            sys::io_uring_queue_init_params, ENOMEM,
            std::errc::not_enough_memory);
        expect(sys::eventfd, EMFILE, std::errc::too_many_files_open);
        // The wakeup poll's submit is the only one ring creation issues.
        expect(sys::io_uring_submit, EBADF, std::errc::bad_file_descriptor);
    }

    /* Every io_uring object type creates and configures its descriptor
       with plain syscalls, and each of those returns rather than
       throwing. None of the error legs had run: the ring's own faults
       reach the operations, not the object surface underneath them.
    */
    void testObjectSurfaceErrors()
    {
        io_context ioc(uring);
        auto expect_open = [&](auto&& obj, auto&& open_it) {
            int const before = open_fds();
            fault_scope f(sys::socket, EMFILE);
            BOOST_TEST(open_it() == std::errc::too_many_files_open);
            BOOST_TEST(f.fired());
            BOOST_TEST(!obj.is_open());
            BOOST_TEST_EQ(open_fds(), before);
        };
        {
            tcp_acceptor acc(ioc);
            expect_open(acc, [&] { return acc.open(); });
            BOOST_TEST(!acc.open());
            BOOST_TEST(!acc.bind(uring_loopback()));
            fault_scope f(sys::listen, EADDRINUSE);
            BOOST_TEST(acc.listen() == std::errc::address_in_use);
            BOOST_TEST(f.fired());
        }
        {
            udp_socket u(ioc);
            expect_open(u, [&] { return u.open(family::v4); });
        }
        {
            local_stream_socket ls(ioc);
            expect_open(ls, [&] { return ls.open(); });
        }
        {
            local_datagram_socket ld(ioc);
            expect_open(ld, [&] { return ld.open(); });
        }
        auto path = temp_path("ura");
        ::unlink(path.c_str());
        {
            local_stream_acceptor acc(ioc);
            expect_open(acc, [&] { return acc.open(); });
            BOOST_TEST(!acc.open());
            BOOST_TEST(!acc.bind(corosio::local_endpoint(path)));
            fault_scope f(sys::listen, EADDRINUSE);
            BOOST_TEST(acc.listen() == std::errc::address_in_use);
            BOOST_TEST(f.fired());
        }
        ::unlink(path.c_str());
        {
            local_datagram_socket ld(ioc);
            BOOST_TEST(!ld.open());
            fault_scope f(sys::bind, EACCES);
            BOOST_TEST(
                ld.bind(corosio::local_endpoint(path)) ==
                std::errc::permission_denied);
            BOOST_TEST(f.fired());
            BOOST_TEST(ld.is_open());
        }
        ::unlink(path.c_str());
        {
            auto [a, b] = test::make_socket_pair(ioc);
            std::ignore = b;
            fault_scope f(sys::shutdown, ENOTCONN);
            BOOST_TEST(a.shutdown(shutdown_both) == std::errc::not_connected);
            BOOST_TEST(f.fired());
        }
        {
            local_stream_socket a(ioc), b(ioc);
            BOOST_TEST(!connect_pair(a, b));
            fault_scope f(sys::shutdown, ENOTCONN);
            BOOST_TEST(a.shutdown(shutdown_both) == std::errc::not_connected);
            BOOST_TEST(f.fired());
        }
        {
            local_datagram_socket a(ioc), b(ioc);
            BOOST_TEST(!connect_pair(a, b));
            fault_scope f(sys::shutdown, ENOTCONN);
            BOOST_TEST(a.shutdown(shutdown_both) == std::errc::not_connected);
            BOOST_TEST(f.fired());
        }
    }

    /* Multishot accept fails immediately on a socket that is not
       listening, so an adopted descriptor is probed before it is
       armed. A kernel that refuses the query is treated as reporting a
       listener, which is what keeps adoption working on a platform
       that has no SO_ACCEPTCONN.
    */
    void testAcceptorAssignProbeFails()
    {
        io_context ioc(uring);
        auto h = make_native_socket(AF_INET, SOCK_STREAM);
        BOOST_TEST(static_cast<int>(h) >= 0);
        make_native_adoptable(h);
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        BOOST_TEST_EQ(
            ::bind(
                static_cast<int>(h), reinterpret_cast<sockaddr*>(&sa),
                sizeof(sa)),
            0);
        BOOST_TEST_EQ(::listen(static_cast<int>(h), 1), 0);
        tcp_acceptor acc(ioc);
        {
            // 1 is the SO_TYPE query adoption validates with; 2 is the
            // SO_ACCEPTCONN probe.
            fault_scope f(sys::getsockopt, EBADF, 2);
            BOOST_TEST(!acc.assign(h));
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(f.count(), 2u);
        }
        BOOST_TEST(acc.is_open());
        acc.close();
    }

    /* A full submission queue does not hold a close.

       A ring clamped to one SQE spends it on the wakeup poll, and the
       flush that would free it is a no-op while the arm is alive, so
       every cancel below finds the queue still full after its own
       retry. Cancels name operations rather than the descriptor, so a
       close still closes at once.
    */
    void testCancelSqFull()
    {
        std::optional<fault_scope> f;
        f.emplace(sys::uring_sqe_full, 0);
        io_context ioc(uring);
        int before = 0;
        {
            tcp_socket s(ioc);
            BOOST_TEST(!s.open(family::v4));
            before = open_fds();
            s.close();
            BOOST_TEST(!s.is_open());
            BOOST_TEST_EQ(open_fds(), before - 1);
        }
        {
            // cancel() reaches the cancel path without going through a
            // close.
            tcp_socket s(ioc);
            BOOST_TEST(!s.open(family::v4));
            s.cancel();
            BOOST_TEST(s.is_open());
        }
        {
            tcp_acceptor acc(ioc);
            BOOST_TEST(!acc.open());
            BOOST_TEST(!acc.bind(uring_loopback()));
            BOOST_TEST(!acc.listen());
        }
        BOOST_TEST(f->fired());
        f.reset();

        // Nothing was left waiting for the queue to drain.
        capy::run_async(ioc.get_executor())(
            []() -> capy::task<> { co_return; }());
        ioc.poll();
        BOOST_TEST_EQ(open_fds(), before - 1);
    }

    void testWaitFails()
    {
        {
            io_context ioc(uring);
            fault_scope f(sys::io_uring_wait_cqe_timeout, EINTR);
            bool done = false;
            auto body = [&]() -> capy::task<> {
                std::ignore =
                    co_await corosio::delay(std::chrono::milliseconds(1));
                done = true;
            };
            capy::run_async(ioc.get_executor())(body());
            ioc.run();
            BOOST_TEST(f.fired());
            BOOST_TEST(done);
        }
        {
            io_context ioc(uring);
            fault_scope f(sys::io_uring_wait_cqe_timeout, EBADF);
            auto body = [&]() -> capy::task<> {
                std::ignore =
                    co_await corosio::delay(std::chrono::milliseconds(1));
            };
            capy::run_async(ioc.get_executor())(body());
            expect_system_error(
                [&] { ioc.run(); }, std::errc::bad_file_descriptor);
            BOOST_TEST(f.fired());
        }
    }

    // Parks a receive in the kernel, so the context's teardown has a
    // request to cancel and wait for.
    static void park_receive(io_context& ioc, udp_socket& s, char* buf)
    {
        BOOST_TEST(!s.open(family::v4));
        BOOST_TEST(!s.bind(endpoint(ipv4_address::loopback(), 0)));
        capy::run_async(ioc.get_executor())(
            [](udp_socket& s, char* b) -> capy::task<> {
                endpoint from;
                std::ignore =
                    co_await s.recv_from(capy::mutable_buffer(b, 8), from);
            }(s, buf));
        ioc.poll();
    }

    // Teardown waits for the kernel to finish every request it holds;
    // a signal interrupting that wait must not end it early, or the
    // services free implementations the kernel can still write to.
    void testShutdownWaitRetriesAfterEintr()
    {
        char buf[8];
        std::optional<fault_scope> first;
        std::optional<fault_scope> again;
        {
            io_context ioc(uring);
            udp_socket s(ioc);
            park_receive(ioc, s, buf);
            first.emplace(sys::io_uring_wait_cqe_timeout, EINTR);
            again.emplace(sys::io_uring_wait_cqe_timeout, EINTR, 2);
        }
        BOOST_TEST(first->fired());
        BOOST_TEST(again->fired()); // the interrupted wait was retried
    }

    // Same for the cancel that teardown submits before waiting.
    void testShutdownCancelRetriesAfterEintr()
    {
        char buf[8];
        std::optional<fault_scope> waits;
        std::optional<fault_scope> submit;
        {
            io_context ioc(uring);
            udp_socket s(ioc);
            park_receive(ioc, s, buf);
            waits.emplace(sys::io_uring_wait_cqe_timeout, EINTR, 1000);
            submit.emplace(sys::io_uring_submit, EINTR);
        }
        BOOST_TEST(submit->fired());
        BOOST_TEST_GE(waits->count(), 1u);
    }

    // A connect whose success arrives after release() belongs to the
    // descriptor the caller took away: it must not leave endpoints on
    // the socket, which may already hold a new descriptor. The kernel's answer is rewritten to success
    // so it lands after the release whatever the cancel did.
    void testReleaseDuringConnect()
    {
        io_context ioc(uring);
        auto ex = ioc.get_executor();
        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open(family::v4));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());

        tcp_socket s(ioc);
        BOOST_TEST(!s.open(family::v4));
        cqe_fault_scope q(-1, IORING_OP_CONNECT, 0);
        bool started = false;
        bool done    = false;
        capy::run_async(ex)(
            [](tcp_socket& s, endpoint ep, bool& started,
               bool& done) -> capy::task<> {
                started     = true;
                std::ignore = co_await s.connect(ep);
                done        = true;
            }(s, acc.local_endpoint(), started, done));
        while (!started && ioc.poll_one() != 0)
        {
        }
        BOOST_TEST(started);

        auto [rel_ec, released] = s.release();
        BOOST_TEST(!rel_ec);
        // The reopened socket must not inherit the old connect's
        // endpoints when its completion runs.
        BOOST_TEST(!s.open(family::v4));
        ioc.restart();
        while (!done && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST(done);
        BOOST_TEST(q.fired());
        BOOST_TEST(s.remote_endpoint() == endpoint{});
        BOOST_TEST(s.local_endpoint() == endpoint{});
        ::close(released);
    }

    // An error wait whose completion runs after the socket was closed
    // and reopened must not probe SO_ERROR through the old descriptor
    // number, which may now name another socket. The cancelled poll's
    // answer is rewritten to an error so it reaches that probe.
    void testErrorWaitAfterCloseSkipsProbe()
    {
        io_context ioc(uring);
        auto ex = ioc.get_executor();
        tcp_socket s(ioc);
        BOOST_TEST(!s.open(family::v4));

        cqe_fault_scope q(-1, IORING_OP_POLL_ADD, POLLERR);
        bool started = false;
        bool done    = false;
        std::error_code wec;
        capy::run_async(ex)(
            [](tcp_socket& s, bool& started, bool& done,
               std::error_code& ec) -> capy::task<> {
                started  = true;
                auto [e] = co_await s.wait(wait_type::error);
                ec       = e;
                done     = true;
            }(s, started, done, wec));
        while (!started && ioc.poll_one() != 0)
        {
        }
        BOOST_TEST(started);

        s.close();
        BOOST_TEST(!s.open(family::v4));
        ioc.restart();
        while (!done && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST(done);
        BOOST_TEST(q.fired());
        BOOST_TEST(wec == capy::cond::canceled);
    }

    // A close whose cancel cannot be queued must not strand the
    // request: the descriptor stays open until the cancel reaches the
    // kernel, and the receive completes canceled.
    void testCloseWithFullQueueStillCancels()
    {
        io_context ioc(uring);
        char buf[8];
        udp_socket s(ioc);
        BOOST_TEST(!s.open(family::v4));
        BOOST_TEST(!s.bind(endpoint(ipv4_address::loopback(), 0)));
        bool done = false;
        std::error_code rec;
        capy::run_async(ioc.get_executor())(
            [](udp_socket& s, char* b, bool& done,
               std::error_code& ec) -> capy::task<> {
                endpoint from;
                auto [e, n] =
                    co_await s.recv_from(capy::mutable_buffer(b, 8), from);
                std::ignore = n;
                ec          = e;
                done        = true;
            }(s, buf, done, rec));
        bool fired  = false;
        auto closer = [&]() -> capy::task<> {
            // A run-loop flush applies the fill; see
            // testCancelSqFullBestEffort.
            fault_scope f(sys::uring_sq_fill, 0);
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(1));
            s.close();
            fired = f.fired();
        };
        capy::run_async(ioc.get_executor())(closer());
        while (!done && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST(fired);
        BOOST_TEST(done);
        BOOST_TEST(rec == capy::cond::canceled);
    }

    // release() hands the descriptor to a caller who may close it at
    // once. A cancel that could not reach the kernel first must still
    // end the parked receive, and must not later cancel whatever
    // reuses the number.
    void testReleaseWithFullQueueStillCancels()
    {
        io_context ioc(uring);
        char buf[8];
        udp_socket s(ioc);
        BOOST_TEST(!s.open(family::v4));
        BOOST_TEST(!s.bind(endpoint(ipv4_address::loopback(), 0)));
        bool done = false;
        std::error_code rec;
        capy::run_async(ioc.get_executor())(
            [](udp_socket& s, char* b, bool& done,
               std::error_code& ec) -> capy::task<> {
                endpoint from;
                auto [e, n] =
                    co_await s.recv_from(capy::mutable_buffer(b, 8), from);
                std::ignore = n;
                ec          = e;
                done        = true;
            }(s, buf, done, rec));
        bool fired    = false;
        auto releaser = [&]() -> capy::task<> {
            // A run-loop flush applies the fill; see
            // testCancelSqFullBestEffort.
            fault_scope f(sys::uring_sq_fill, 0);
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(1));
            auto [rel_ec, rel_fd] = s.release();
            BOOST_TEST(!rel_ec);
            ::close(rel_fd);
            fired = f.fired();
        };
        capy::run_async(ioc.get_executor())(releaser());
        while (!done && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST(fired);
        BOOST_TEST(done);
        BOOST_TEST(rec == capy::cond::canceled);
    }

    // A release that could not queue its cancel still hands back the
    // descriptor itself and cancels the receive later. That cancel names
    // the receive, so another object adopting the descriptor keeps its
    // own receive.
    void testDeferredCancelSparesReadoptedDescriptor()
    {
        io_context ioc(uring);
        char buf[8];
        udp_socket s(ioc);
        BOOST_TEST(!s.open(family::v4));
        BOOST_TEST(!s.bind(endpoint(ipv4_address::loopback(), 0)));
        int const orig = static_cast<int>(s.native_handle());
        bool done1     = false;
        std::error_code rec1;
        capy::run_async(ioc.get_executor())(
            [](udp_socket& s, char* b, bool& done,
               std::error_code& ec) -> capy::task<> {
                endpoint from;
                auto [e, n] =
                    co_await s.recv_from(capy::mutable_buffer(b, 8), from);
                std::ignore = n;
                ec          = e;
                done        = true;
            }(s, buf, done1, rec1));

        udp_socket s2(ioc);
        char buf2[8];
        bool done2 = false;
        std::error_code rec2;
        std::size_t n2 = 0;
        int released   = -1;
        bool fired     = false;
        auto releaser  = [&]() -> capy::task<> {
            {
                // A run-loop flush applies the fill; see
                // testCancelSqFullBestEffort.
                fault_scope f(sys::uring_sq_fill, 0);
                std::ignore =
                    co_await corosio::delay(std::chrono::milliseconds(1));
                auto [rel_ec, rel_fd] = s.release();
                BOOST_TEST(!rel_ec);
                released = rel_fd;
                fired    = f.fired();
            }
            BOOST_TEST(!s2.assign(released));
            endpoint from;
            auto [e, n] =
                co_await s2.recv_from(capy::mutable_buffer(buf2, 8), from);
            rec2  = e;
            n2    = n;
            done2 = true;
        };
        capy::run_async(ioc.get_executor())(releaser());
        while (!done1 && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST(fired);
        BOOST_TEST_EQ(released, orig);
        BOOST_TEST(done1);
        BOOST_TEST(rec1 == capy::cond::canceled);

        // The deferred cancel has run; the adopter's receive must still
        // be waiting for data.
        udp_socket sender(ioc);
        BOOST_TEST(!sender.open(family::v4));
        auto to = s2.local_endpoint();
        capy::run_async(ioc.get_executor())(
            [](udp_socket& u, endpoint ep) -> capy::task<> {
                std::ignore =
                    co_await u.send_to(capy::const_buffer("hi", 2), ep);
            }(sender, to));
        while (!done2 && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST(done2);
        BOOST_TEST(!rec2);
        BOOST_TEST_EQ(n2, 2u);
        s2.close();
        ioc.restart();
        ioc.run();
    }

    // A close whose flush fails must not leave an SQE that still names
    // the descriptor by number: the queued receive completes canceled
    // before the number can be reused.
    void testCloseWithFailedFlushCancelsQueuedOp()
    {
        io_context ioc(uring);
        char buf[8];
        udp_socket s(ioc);
        BOOST_TEST(!s.open(family::v4));
        BOOST_TEST(!s.bind(endpoint(ipv4_address::loopback(), 0)));
        bool done = false;
        std::error_code rec;
        bool fired = false;
        // The receive links its SQE and posts the flush behind the
        // closer, which therefore closes with the SQE still queued.
        capy::run_async(ioc.get_executor())(
            [](udp_socket& s, char* b, bool& done,
               std::error_code& ec) -> capy::task<> {
                endpoint from;
                auto [e, n] =
                    co_await s.recv_from(capy::mutable_buffer(b, 8), from);
                std::ignore = n;
                ec          = e;
                done        = true;
            }(s, buf, done, rec));
        capy::run_async(ioc.get_executor())(
            [](udp_socket& s, bool& fired) -> capy::task<> {
                fault_scope f(sys::io_uring_submit, ENOMEM);
                s.close();
                fired = f.fired();
                co_return;
            }(s, fired));
        while (!done && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST(fired);
        BOOST_TEST(done);
        BOOST_TEST(rec == capy::cond::canceled);
    }

    // Closing and destroying a listening acceptor on a live context
    // leaves the arming's terminal completion to the run loop. Waiting
    // on the ring for it instead would swallow other ops' completions;
    // the kernel wait is the observable and must not be reached.
    void testAcceptorCloseDoesNotWaitOnRing()
    {
        io_context ioc(uring);
        fault_scope f(sys::io_uring_submit_and_wait_timeout, EBADF);
        {
            tcp_acceptor acc(ioc, uring_loopback());
            tcp_socket s(ioc);
            std::error_code aec;
            auto accept_body = [&]() -> capy::task<> {
                auto [ec] = co_await acc.accept(s);
                aec       = ec;
            };
            capy::run_async(ioc.get_executor())(accept_body());
            ioc.poll();
            acc.close();
            ioc.poll();
            BOOST_TEST(aec == capy::error::canceled);
        }
        BOOST_TEST(!f.fired());
    }

    // A ring clamped to one SQE spends it on the wakeup poll, and the
    // deferred flush that would free it is failed too, so the first
    // user op finds the SQ still full after its own flush retry. Every
    // op kind reports the same EAGAIN there, whatever it was going to
    // do with the SQE. The pairs are built with raw syscalls and
    // adopted: connecting through a ring this crippled would deadlock
    // before the test began.
    void testSqFull()
    {
        // A read that never reached the kernel is not an end of file.
        {
            int sv[2];
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
            {
                BOOST_TEST(false);
                return;
            }
            make_native_adoptable(sv[1]);
            std::optional<fault_scope> f;
            f.emplace(sys::uring_sqe_full, 0);
            fault_scope g(sys::io_uring_submit_and_get_events, EBADF);
            io_context ioc(uring);
            local_stream_socket b(ioc);
            BOOST_TEST(!b.assign(sv[1]));
            char buf[8];
            std::error_code rec, rec2;
            std::size_t n1 = 1, n2 = 0;
            auto read_body = [&]() -> capy::task<> {
                {
                    auto [ec, n] =
                        co_await b.read_some(capy::mutable_buffer(buf, 8));
                    rec = ec;
                    n1  = n;
                }
                // Nothing was consumed, so the peer's next bytes still
                // arrive: the follow-up read needs no SQE because the
                // speculative readv answers it.
                BOOST_TEST_EQ(::write(sv[0], "abcd", 4), 4);
                {
                    auto [ec, n] =
                        co_await b.read_some(capy::mutable_buffer(buf, 8));
                    rec2 = ec;
                    n2   = n;
                }
            };
            capy::run_async(ioc.get_executor())(read_body());
            ioc.run();
            BOOST_TEST(f->fired());
            // A real flush would take shutdown's own cancel.
            f.reset();
            BOOST_TEST(g.fired());
            BOOST_TEST(rec == std::errc::resource_unavailable_try_again);
            BOOST_TEST_EQ(n1, 0u);
            BOOST_TEST(b.is_open());
            BOOST_TEST(!rec2);
            BOOST_TEST_EQ(n2, 4u);
            BOOST_TEST_EQ(std::memcmp(buf, "abcd", 4), 0);
            ::close(sv[0]);
        }
        // A write that never reached the kernel moved no bytes, so
        // reporting success with a zero count would lose the payload.
        {
            int sv[2];
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
            {
                BOOST_TEST(false);
                return;
            }
            make_native_adoptable(sv[1]);
            std::optional<fault_scope> f;
            f.emplace(sys::uring_sqe_full, 0);
            fault_scope g(sys::io_uring_submit_and_get_events, EBADF);
            io_context ioc(uring);
            local_stream_socket b(ioc);
            BOOST_TEST(!b.assign(sv[1]));
            std::error_code wec;
            std::size_t wn  = 1;
            auto write_body = [&]() -> capy::task<> {
                // The socket is writable, so the speculative sendmsg
                // would answer the write without ever needing an SQE.
                fault_scope s(sys::sendmsg, EAGAIN);
                auto [ec, n] =
                    co_await b.write_some(capy::const_buffer("abcd", 4));
                wec = ec;
                wn  = n;
                BOOST_TEST(s.fired());
            };
            capy::run_async(ioc.get_executor())(write_body());
            ioc.run();
            BOOST_TEST(f->fired());
            // A real flush would take shutdown's own cancel.
            f.reset();
            BOOST_TEST(g.fired());
            BOOST_TEST(wec == std::errc::resource_unavailable_try_again);
            BOOST_TEST_EQ(wn, 0u);
            BOOST_TEST(b.is_open());
            ::close(sv[0]);
        }
        // A readiness wait carries no byte count to give it away: an
        // unsubmitted poll that reported success would have the caller
        // read a socket nothing said was readable.
        {
            int sv[2];
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
            {
                BOOST_TEST(false);
                return;
            }
            make_native_adoptable(sv[1]);
            std::optional<fault_scope> f;
            f.emplace(sys::uring_sqe_full, 0);
            fault_scope g(sys::io_uring_submit_and_get_events, EBADF);
            io_context ioc(uring);
            local_stream_socket b(ioc);
            BOOST_TEST(!b.assign(sv[1]));
            std::error_code pec;
            auto wait_body = [&]() -> capy::task<> {
                auto [ec] = co_await b.wait(wait_type::read);
                pec       = ec;
            };
            capy::run_async(ioc.get_executor())(wait_body());
            ioc.run();
            BOOST_TEST(f->fired());
            // A real flush would take shutdown's own cancel.
            f.reset();
            BOOST_TEST(g.fired());
            BOOST_TEST(pec == std::errc::resource_unavailable_try_again);
            BOOST_TEST(b.is_open());
            ::close(sv[0]);
        }
    }

    void testConnectCqeRewrite()
    {
        io_context ioc(uring);
        tcp_acceptor acc(ioc, uring_loopback());
        tcp_socket c(ioc);
        BOOST_TEST(!c.open(family::v4));
        std::error_code cec;
        bool fired = false;
        {
            cqe_fault_scope q(
                c.native_handle(), IORING_OP_CONNECT, -ECONNREFUSED);
            auto connect_body = [&]() -> capy::task<> {
                auto [ec] = co_await c.connect(acc.local_endpoint());
                cec       = ec;
            };
            capy::run_async(ioc.get_executor())(connect_body());
            ioc.run();
            fired = q.fired();
        }
        BOOST_TEST(fired);
        BOOST_TEST(cec == std::errc::connection_refused);
        // A refused connect reports through the return channel and
        // leaves the descriptor with the caller.
        BOOST_TEST(c.is_open());
    }

    // The multishot accept SQE is prepared by listen(), so the arm has
    // to be in place before the listener is armed rather than before
    // the accept call.
    void testAcceptCqeRewrite()
    {
        io_context ioc(uring);
        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(uring_loopback()));
        tcp_socket c(ioc), s(ioc);
        std::error_code aec;
        bool fired = false;
        {
            cqe_fault_scope q(acc.native_handle(), IORING_OP_ACCEPT, -EMFILE);
            BOOST_TEST(!acc.listen());
            auto accept_body = [&]() -> capy::task<> {
                auto [ec] = co_await acc.accept(s);
                aec       = ec;
            };
            capy::run_async(ioc.get_executor())(accept_body());
            auto connect_body = [&]() -> capy::task<> {
                std::ignore = co_await c.connect(acc.local_endpoint());
            };
            capy::run_async(ioc.get_executor())(connect_body());
            ioc.run();
            fired = q.fired();
        }
        BOOST_TEST(fired);
        BOOST_TEST(aec == std::errc::too_many_files_open);
        // A failed accept adopts nothing into the peer socket.
        BOOST_TEST(!s.is_open());
    }

    // Stream reads and writes try a synchronous readv/sendmsg before
    // they submit, so each rewrite needs the speculation pushed off the
    // fast path with its own EAGAIN arm.
    void testStreamCqeRewrites()
    {
        io_context ioc(uring);
        auto [a, b] = test::make_socket_pair(ioc);
        char buf[8] = "1234567";
        std::error_code sec, rec, pec, smec, rvec;
        bool sfired = false, rfired = false, pfired = false;
        bool smfired = false, rvfired = false;
        auto body = [&]() -> capy::task<> {
            {
                fault_scope f(sys::sendmsg, EAGAIN);
                cqe_fault_scope q(a.native_handle(), IORING_OP_SEND, -EPIPE);
                auto [ec, n] =
                    co_await a.write_some(capy::const_buffer(buf, 7));
                std::ignore = n;
                sec         = ec;
                BOOST_TEST(f.fired());
                sfired = q.fired();
            }
            {
                fault_scope f(sys::readv, EAGAIN);
                cqe_fault_scope q(
                    b.native_handle(), IORING_OP_RECV, -ECONNRESET);
                auto [ec, n] =
                    co_await b.read_some(capy::mutable_buffer(buf, 7));
                std::ignore = n;
                rec         = ec;
                BOOST_TEST(f.fired());
                rfired = q.fired();
            }
            // The two faulted completions carried a negative res, which
            // never re-arms speculation, so the scatter-gather pair
            // below reaches its SQE without an EAGAIN arm of its own.
            {
                cqe_fault_scope q(
                    a.native_handle(), IORING_OP_SENDMSG, -ENOBUFS);
                std::array<capy::const_buffer, 2> cb{
                    capy::const_buffer(buf, 4), capy::const_buffer(buf + 4, 3)};
                auto [ec, n] = co_await a.write_some(cb);
                std::ignore  = n;
                smec         = ec;
                smfired      = q.fired();
            }
            {
                char x[4] = {}, y[4] = {};
                cqe_fault_scope q(b.native_handle(), IORING_OP_READV, -EIO);
                std::array<capy::mutable_buffer, 2> mb{
                    capy::mutable_buffer(x, 4), capy::mutable_buffer(y, 3)};
                auto [ec, n] = co_await b.read_some(mb);
                std::ignore  = n;
                rvec         = ec;
                rvfired      = q.fired();
            }
            {
                // The rewritten RECV really ran, so b's buffer is empty
                // again; the poll needs data on the other side or it
                // would park forever instead of delivering a CQE.
                auto [ec, n] =
                    co_await b.write_some(capy::const_buffer(buf, 7));
                std::ignore = n;
                BOOST_TEST(!ec);
            }
            {
                cqe_fault_scope q(
                    a.native_handle(), IORING_OP_POLL_ADD, -EBADF);
                auto [ec] = co_await a.wait(wait_type::read);
                pec       = ec;
                pfired    = q.fired();
            }
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST(sfired);
        BOOST_TEST(rfired);
        BOOST_TEST(smfired);
        BOOST_TEST(rvfired);
        BOOST_TEST(pfired);
        BOOST_TEST(sec == std::errc::broken_pipe);
        BOOST_TEST(rec == std::errc::connection_reset);
        BOOST_TEST(smec == std::errc::no_buffer_space);
        BOOST_TEST(rvec == std::errc::io_error);
        BOOST_TEST(pec == std::errc::bad_file_descriptor);
    }

    void testDatagramCqeRewrites()
    {
        io_context ioc(uring);
        udp_socket a(ioc), b(ioc);
        BOOST_TEST(!a.open(family::v4));
        BOOST_TEST(!b.open(family::v4));
        BOOST_TEST(!a.bind(uring_loopback()));
        BOOST_TEST(!b.bind(uring_loopback()));
        char buf[8] = "1234567";
        std::error_code sec, rec;
        bool sfired = false, rfired = false;
        auto body = [&]() -> capy::task<> {
            {
                fault_scope f(sys::sendmsg, EAGAIN);
                cqe_fault_scope q(a.native_handle(), IORING_OP_SENDMSG, -EIO);
                auto [ec, n] = co_await a.send_to(
                    capy::const_buffer(buf, 7), b.local_endpoint());
                std::ignore = n;
                sec         = ec;
                BOOST_TEST(f.fired());
                sfired = q.fired();
            }
            {
                fault_scope f(sys::recvmsg, EAGAIN);
                cqe_fault_scope q(b.native_handle(), IORING_OP_RECVMSG, -EIO);
                endpoint from;
                auto [ec, n] =
                    co_await b.recv_from(capy::mutable_buffer(buf, 7), from);
                std::ignore = n;
                rec         = ec;
                BOOST_TEST(f.fired());
                rfired = q.fired();
            }
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST(sfired);
        BOOST_TEST(rfired);
        BOOST_TEST(sec == std::errc::io_error);
        BOOST_TEST(rec == std::errc::io_error);
    }

    // Ring file I/O never reaches preadv/pwritev, so the failure that
    // the POSIX file tests inject through those calls is only
    // reachable here, on the completion of the READV/WRITEV SQE.
    void testFileCqeRewrites()
    {
        io_context ioc(uring);
        auto sf_path = temp_path("uring_sf");
        auto rf_path = temp_path("uring_raf");
        stream_file sf(ioc);
        random_access_file rf(ioc);
        BOOST_TEST(
            !sf.open(sf_path, file_base::read_write | file_base::create));
        BOOST_TEST(
            !rf.open(rf_path, file_base::read_write | file_base::create));
        char buf[8] = "1234567";
        std::error_code swec, srec, rwec, rrec;
        bool swf = false, srf = false, rwf = false, rrf = false;
        auto body = [&]() -> capy::task<> {
            {
                cqe_fault_scope q(sf.native_handle(), IORING_OP_WRITEV, -EIO);
                auto [ec, n] =
                    co_await sf.write_some(capy::const_buffer(buf, 7));
                std::ignore = n;
                swec        = ec;
                swf         = q.fired();
            }
            {
                // A faulted completion is not sticky: the file still
                // takes writes, and this one gives the read below
                // something to find.
                auto [ec, n] =
                    co_await sf.write_some(capy::const_buffer(buf, 7));
                std::ignore = n;
                BOOST_TEST(!ec);
            }
            {
                auto [ec, pos] = sf.seek(0, file_base::seek_set);
                std::ignore    = pos;
                BOOST_TEST(!ec);
            }
            {
                cqe_fault_scope q(sf.native_handle(), IORING_OP_READV, -EIO);
                auto [ec, n] =
                    co_await sf.read_some(capy::mutable_buffer(buf, 7));
                std::ignore = n;
                srec        = ec;
                srf         = q.fired();
            }
            {
                cqe_fault_scope q(rf.native_handle(), IORING_OP_WRITEV, -EIO);
                auto [ec, n] =
                    co_await rf.write_some_at(0, capy::const_buffer(buf, 7));
                std::ignore = n;
                rwec        = ec;
                rwf         = q.fired();
            }
            {
                // Same for the random-access file: the faulted
                // completion leaves it usable, and this write seeds the
                // read below.
                auto [ec, n] =
                    co_await rf.write_some_at(0, capy::const_buffer(buf, 7));
                std::ignore = n;
                BOOST_TEST(!ec);
            }
            {
                cqe_fault_scope q(rf.native_handle(), IORING_OP_READV, -EIO);
                auto [ec, n] =
                    co_await rf.read_some_at(0, capy::mutable_buffer(buf, 7));
                std::ignore = n;
                rrec        = ec;
                rrf         = q.fired();
            }
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST(swf);
        BOOST_TEST(srf);
        BOOST_TEST(rwf);
        BOOST_TEST(rrf);
        BOOST_TEST(swec == std::errc::io_error);
        BOOST_TEST(srec == std::errc::io_error);
        BOOST_TEST(rwec == std::errc::io_error);
        BOOST_TEST(rrec == std::errc::io_error);
        sf.close();
        rf.close();
        ::unlink(sf_path.c_str());
        ::unlink(rf_path.c_str());
    }

    /* The multishot accept arm is the one submission nothing counted.

       Every other op is paid for by a `work_started()` before it is
       submitted, which is what lets the SQ-full path queue it as
       completed: the run loop spends a `work_finished()` on everything
       it dispatches. The arm is a persistent internal mechanism with no
       such credit, so queueing it would spend one the context never
       owed. Its failure comes back through the acceptor instead, which
       is also the only channel that can say anything at all: an arm the
       ring never took produces no CQE, so an accept that parked on a
       delivery would park for good.
    */
    void testAcceptorArmSqFull()
    {
        std::optional<fault_scope> f, g;
        f.emplace(sys::uring_sqe_full, 0);
        g.emplace(sys::io_uring_submit_and_get_events, EBADF);
        io_context ioc(uring);
        // The listen inside the constructor is what arms the multishot
        // accept, and it finds the one SQE already spent on the wakeup
        // poll.
        tcp_acceptor acc(ioc, uring_loopback());
        tcp_socket s(ioc);
        std::error_code aec, wec;
        auto body = [&]() -> capy::task<> {
            {
                auto [ec] = co_await acc.accept(s);
                aec       = ec;
            }
            {
                // Readiness on this backend means a future delivery,
                // which the failed arm has ruled out just as squarely.
                auto [ec] = co_await acc.wait(wait_type::read);
                wec       = ec;
            }
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST(f->fired());
        // The deferred flush is failed too, so the SQ is still full
        // when the arm makes its own retry.
        BOOST_TEST(g->fired());
        BOOST_TEST(aec == std::errc::resource_unavailable_try_again);
        BOOST_TEST(wec == std::errc::resource_unavailable_try_again);
        BOOST_TEST(!s.is_open());

        // A failed arm has to be recoverable, or an acceptor that hit
        // a full SQ once would report EAGAIN for the rest of its life:
        // the arming it is left holding covers nothing, so a re-listen
        // must be allowed to replace it rather than being read as one
        // already in place.
        f.reset();
        g.reset();
        ioc.restart();
        BOOST_TEST(!acc.listen());
        tcp_socket c(ioc), s2(ioc);
        std::error_code cec, aec2;
        // The accept goes first and finds nothing queued, so the
        // synchronous accept4 answers EAGAIN and the connection can
        // only reach it through the arming. That is what makes this an
        // assertion about the arming rather than about accept4: with
        // the failed arm still counted as one in place, listen() would
        // not have replaced it and this accept would report EAGAIN
        // instead of parking.
        auto accept_body = [&]() -> capy::task<> {
            auto [ec] = co_await acc.accept(s2);
            aec2      = ec;
        };
        auto client_body = [&]() -> capy::task<> {
            auto [ec] = co_await c.connect(acc.local_endpoint());
            cec       = ec;
        };
        capy::run_async(ioc.get_executor())(accept_body());
        capy::run_async(ioc.get_executor())(client_body());
        ioc.run();
        BOOST_TEST(!cec);
        BOOST_TEST(!aec2);
        BOOST_TEST(s2.is_open());
    }

    // Ring construction with no SQE for the wakeup poll must tear the
    // ring back down and throw rather than run without a wake path.
    void testRingInitSqExhaustion()
    {
        fault_scope f(sys::uring_sq_fill, 0);
        BOOST_TEST_THROWS(([] { io_context tmp(uring); }()), std::system_error);
        BOOST_TEST(f.fired());
    }

    // Cancellation is best-effort when the submission queue stays
    // full after one flush: the op stays parked, and closing the
    // socket still completes it.
    void testCancelSqFullBestEffort()
    {
        io_context ioc(uring);
        auto ex = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        std::stop_source ss;
        char buf[8];
        std::error_code rec;
        bool done   = false;
        auto reader = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            std::ignore = n;
            rec         = ec;
            done        = true;
        };
        auto stopper = [&]() -> capy::task<> {
            // The arm needs a run-loop flush to apply the fill before
            // the stop callback submits its cancel inline; the delay
            // hop provides one, and the scope legally spans the awaits
            // on a single-threaded context.
            fault_scope f(sys::uring_sq_fill, 0);
            // A real suspension: a zero delay can complete inline
            // without the run-loop flush that applies the fill.
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(1));
            ss.request_stop();
            BOOST_TEST(f.fired());
        };
        capy::run_async(ex, ss.get_token())(reader());
        capy::run_async(ex)(stopper());
        // The skipped cancel leaves the read parked; the bounded run
        // lets everything else settle, then the close completes it.
        std::ignore = ioc.run_for(std::chrono::seconds(2));
        s1.close();
        ioc.restart();
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(rec == capy::cond::canceled);
    }

    // A multishot accept whose arming cannot get an SQE must complete
    // the parked waiter with the error instead of parking it forever.
    void testMultishotArmFailure()
    {
        io_context ioc(uring);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open(family::v4));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());

        tcp_socket peer(ioc);
        std::error_code aec;
        bool done   = false;
        bool fired  = false;
        auto arming = [&]() -> capy::task<> {
            fault_scope f(sys::uring_sq_fill, 0);
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(0));
            // The fill is applied; initiate the accept while the
            // submission queue reads full so the multishot arming
            // takes the failure path with the waiter parked.
            auto [ec] = co_await acc.accept(peer);
            aec       = ec;
            done      = true;
            fired     = f.fired();
        };
        capy::run_async(ex)(arming());
        std::ignore = ioc.run_for(std::chrono::seconds(2));
        if (!done)
        {
            // The arming path did not consume the waiter; close so
            // the run can finish and the assertions report it.
            acc.close();
            ioc.restart();
            ioc.run();
        }

        BOOST_TEST(done);
        BOOST_TEST(fired);
        BOOST_TEST(!!aec);
        BOOST_TEST(!peer.is_open());
    }

    // A multishot accept the kernel terminates on its own (F_MORE
    // cleared, no fatal error) is re-armed, so later connections are
    // still accepted.
    void testMultishotRearmKeepsAccepting()
    {
        io_context ioc(uring);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open(family::v4));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        cqe_fault_scope q(-1, IORING_OP_ACCEPT, -EAGAIN, IORING_CQE_F_MORE);
        BOOST_TEST(!acc.listen());

        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_port        = htons(acc.local_endpoint().port());
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        auto connect_raw   = [&] {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            BOOST_TEST_EQ(
                ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)), 0);
            return fd;
        };

        // The first connection provokes the rewritten terminal CQE.
        int c1 = connect_raw();
        for (int i = 0; i < 10000 && !q.fired(); ++i)
        {
            capy::run_async(ex)([]() -> capy::task<> { co_return; }());
            ioc.restart();
            ioc.poll();
        }
        BOOST_TEST(q.fired());

        int c2 = connect_raw();
        tcp_socket peer(ioc);
        bool accepted = false;
        capy::run_async(ex)(
            [](tcp_acceptor& a, tcp_socket& p, bool& done) -> capy::task<> {
                auto [ec] = co_await a.accept(p);
                done      = !ec;
            }(acc, peer, accepted));
        ioc.restart();
        std::ignore = ioc.run_for(std::chrono::seconds(2));
        if (!accepted)
        {
            acc.close();
            ioc.restart();
            ioc.run();
        }
        BOOST_TEST(accepted);

        ::close(c1);
        ::close(c2);
    }

    // A multishot accept the kernel terminates (F_MORE cleared, no
    // fatal error) is re-armed; a re-arm that cannot get an SQE must
    // drain the parked waiters with the error rather than strand them.
    void testMultishotRearmFailureWithWaiter()
    {
        io_context ioc(uring);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open(family::v4));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));

        // The multishot arms at listen(), so the scope watches before
        // that: the arming SQE's CQE is rewritten to a spurious result
        // with the termination bit cleared.
        cqe_fault_scope q(-1, IORING_OP_ACCEPT, -EAGAIN, IORING_CQE_F_MORE);
        BOOST_TEST(!acc.listen());

        tcp_socket peer(ioc);
        std::error_code aec;
        bool done     = false;
        auto accepter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.accept(peer);
            aec       = ec;
            done      = true;
        };
        auto trip = [&]() -> capy::task<> {
            // Keep the queue full across the re-arm the rewritten CQE
            // provokes; the raw connect generates that CQE.
            fault_scope f(sys::uring_sq_fill, 0);
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(1));
            int fd      = ::socket(AF_INET, SOCK_STREAM, 0);
            BOOST_TEST_GE(fd, 0);
            sockaddr_in sa{};
            sa.sin_family      = AF_INET;
            sa.sin_port        = htons(acc.local_endpoint().port());
            sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            BOOST_TEST_EQ(
                ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)), 0);
            // The rewritten CQE and the failed re-arm happen while the
            // fill is held; the hop gives the run loop a chance to
            // reach both.
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));
            ::close(fd);
        };
        capy::run_async(ex)(accepter());
        capy::run_async(ex)(trip());
        std::ignore = ioc.run_for(std::chrono::seconds(2));
        if (!done)
        {
            acc.close();
            ioc.restart();
            ioc.run();
        }

        BOOST_TEST(done);
        BOOST_TEST(q.fired());
        BOOST_TEST(!!aec);
    }

    // The kernel may clear IORING_CQE_F_MORE on the wakeup eventfd's
    // multishot poll; the scheduler must re-arm it or every later
    // cross-thread wake is lost.
    void testWakeupPollRearm()
    {
        cqe_fault_scope q(-1, IORING_OP_POLL_ADD, 1, IORING_CQE_F_MORE);
        io_context ioc(uring);
        auto ex = ioc.get_executor();

        std::atomic<int> ran{0};
        for (int round = 0; round < 2; ++round)
        {
            std::thread poster([&] {
                // The counter travels as a parameter: the poster's
                // stack (and any closure on it) is gone before the
                // run loop executes the frame.
                capy::run_async(ex)([](std::atomic<int>* n) -> capy::task<> {
                    n->fetch_add(1);
                    co_return;
                }(&ran));
            });
            poster.join();
            ioc.restart();
            std::ignore = ioc.run();
        }
        int const total = ran.load();
        BOOST_TEST(q.fired());
        BOOST_TEST_EQ(total, 2);
    }

    // Same termination on the signal pipe's poll, during a run and
    // during the shutdown drain. The process-global signal state must
    // not leak into the parent, or the signal-pipe fault suite later
    // in this binary finds the pipe already made and its arms never
    // fire; the bodies fork.
    void testSignalPipePollRearm()
    {
        in_child([] {
            io_context ioc(uring);
            auto ex = ioc.get_executor();
            // Armed after the ring exists, so the first pending
            // POLL_ADD the scope matches is the signal pipe's.
            cqe_fault_scope q(-1, IORING_OP_POLL_ADD, 1, IORING_CQE_F_MORE);
            signal_set sigs(ioc);
            if (sigs.add(SIGUSR1))
                return false;

            int got   = 0;
            auto task = [&]() -> capy::task<> {
                auto [ec, sig] = co_await sigs.wait();
                if (!ec)
                    got = sig;
            };
            capy::run_async(ex)(task());
            std::raise(SIGUSR1);
            ioc.run();
            if (got != SIGUSR1 || !q.fired())
                return false;

            // The rearm must keep delivery alive.
            int got2   = 0;
            auto task2 = [&]() -> capy::task<> {
                auto [ec, sig] = co_await sigs.wait();
                if (!ec)
                    got2 = sig;
            };
            ioc.restart();
            capy::run_async(ex)(task2());
            std::raise(SIGUSR1);
            ioc.run();
            return got2 == SIGUSR1;
        });
        in_child([] {
            // A terminated pipe poll arriving in the teardown drain.
            io_context ioc(uring);
            cqe_fault_scope q(-1, IORING_OP_POLL_ADD, 1, IORING_CQE_F_MORE);
            signal_set sigs(ioc);
            if (sigs.add(SIGUSR1))
                return false;
            std::raise(SIGUSR1);
            return true;
        });
    }

    // Under unsafe_io one thread runs the ring, and when it arms a timer
    // itself it is not in a kernel wait: waking it costs a syscall per
    // arm for nothing.
    void testUnsafeIoRunThreadArmsWithoutWake()
    {
        io_context_options opts;
        opts.locking = locking_mode::unsafe_io;
        io_context ioc(uring, opts, 1);
        constexpr int arms = 200;
        capy::run_async(ioc.get_executor())([]() -> capy::task<> {
            for (int i = 0; i < arms; ++i)
                std::ignore =
                    co_await corosio::delay(std::chrono::microseconds(1));
        }());
        // Counts this thread's writes; the call it would fail is never
        // reached.
        fault_scope writes(sys::write, EIO, 1u << 30);
        ioc.run();
        BOOST_TEST_LT(writes.count(), unsigned(arms / 10));
    }

    void run()
    {
        testRingInitSqExhaustion();
        testCancelSqFullBestEffort();
        testMultishotArmFailure();
        testMultishotRearmKeepsAccepting();
        testMultishotRearmFailureWithWaiter();
        testWakeupPollRearm();
        testSignalPipePollRearm();
        if (skip_under_valgrind())
            return;
        testRingInitFails();
        testObjectSurfaceErrors();
        testAcceptorAssignProbeFails();
        testCancelSqFull();
        testWaitFails();
        testAcceptorCloseDoesNotWaitOnRing();
        testShutdownWaitRetriesAfterEintr();
        testShutdownCancelRetriesAfterEintr();
        testReleaseDuringConnect();
        testErrorWaitAfterCloseSkipsProbe();
        testCloseWithFullQueueStillCancels();
        testReleaseWithFullQueueStillCancels();
        testDeferredCancelSparesReadoptedDescriptor();
        testCloseWithFailedFlushCancelsQueuedOp();
        testUnsafeIoRunThreadArmsWithoutWake();
        testAcceptorArmSqFull();
        testSqFull();
        testConnectCqeRewrite();
        testAcceptCqeRewrite();
        testStreamCqeRewrites();
        testDatagramCqeRewrites();
        testFileCqeRewrites();
    }
};

TEST_SUITE(uring_faults, "boost.corosio.fault.uring");

} // namespace boost::corosio::test::fault

#endif
