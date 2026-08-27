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
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <optional>
#include <system_error>
#include <tuple>

#if BOOST_COROSIO_HAS_IO_URING

#include <liburing.h>

#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::test::fault {

namespace {

endpoint uring_loopback()
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
        auto expect = [](sys s, int err, std::errc code)
        {
            int const before = open_fds();
            fault_scope f(s, err);
            expect_system_error([&]{ io_context ioc(io_uring); }, code);
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(open_fds(), before);
        };
        expect(sys::io_uring_queue_init_params, ENOMEM,
            std::errc::not_enough_memory);
        expect(sys::eventfd, EMFILE, std::errc::too_many_files_open);
        // The wakeup poll's submit is the only one ring creation issues.
        expect(sys::io_uring_submit, EBADF,
            std::errc::bad_file_descriptor);
    }

    void testWaitFails()
    {
        {
            io_context ioc(io_uring);
            fault_scope f(sys::io_uring_wait_cqe_timeout, EINTR);
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
            io_context ioc(io_uring);
            fault_scope f(sys::io_uring_wait_cqe_timeout, EBADF);
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

    // The acceptor's close path drains its multishot CQEs; a failing
    // submit_and_wait_timeout breaks that loop silently, so the only
    // observable is that the fault was reached.
    void testAcceptorDrainSubmitFails()
    {
        io_context ioc(io_uring);
        fault_scope f(sys::io_uring_submit_and_wait_timeout, EBADF);
        {
            tcp_acceptor acc(ioc, uring_loopback());
            tcp_socket s(ioc);
            std::error_code aec;
            auto accept_body = [&]() -> capy::task<>
            {
                auto [ec] = co_await acc.accept(s);
                aec = ec;
            };
            capy::run_async(ioc.get_executor())(accept_body());
            ioc.poll();
            acc.close();
            ioc.poll();
            BOOST_TEST(aec == capy::error::canceled);
        }
        BOOST_TEST(f.fired());
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
            if(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
            {
                BOOST_TEST(false);
                return;
            }
            make_native_adoptable(sv[1]);
            fault_scope f(sys::uring_sqe_full, 0);
            fault_scope g(sys::io_uring_submit_and_get_events, EBADF);
            io_context ioc(io_uring);
            local_stream_socket b(ioc);
            BOOST_TEST(!b.assign(sv[1]));
            char buf[8];
            std::error_code rec, rec2;
            std::size_t n1 = 1, n2 = 0;
            auto read_body = [&]() -> capy::task<>
            {
                {
                    auto [ec, n] = co_await b.read_some(
                        capy::mutable_buffer(buf, 8));
                    rec = ec;
                    n1 = n;
                }
                // Nothing was consumed, so the peer's next bytes still
                // arrive: the follow-up read needs no SQE because the
                // speculative readv answers it.
                BOOST_TEST_EQ(::write(sv[0], "abcd", 4), 4);
                {
                    auto [ec, n] = co_await b.read_some(
                        capy::mutable_buffer(buf, 8));
                    rec2 = ec;
                    n2 = n;
                }
            };
            capy::run_async(ioc.get_executor())(read_body());
            ioc.run();
            BOOST_TEST(f.fired());
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
            if(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
            {
                BOOST_TEST(false);
                return;
            }
            make_native_adoptable(sv[1]);
            fault_scope f(sys::uring_sqe_full, 0);
            fault_scope g(sys::io_uring_submit_and_get_events, EBADF);
            io_context ioc(io_uring);
            local_stream_socket b(ioc);
            BOOST_TEST(!b.assign(sv[1]));
            std::error_code wec;
            std::size_t wn = 1;
            auto write_body = [&]() -> capy::task<>
            {
                // The socket is writable, so the speculative sendmsg
                // would answer the write without ever needing an SQE.
                fault_scope s(sys::sendmsg, EAGAIN);
                auto [ec, n] = co_await b.write_some(
                    capy::const_buffer("abcd", 4));
                wec = ec;
                wn = n;
                BOOST_TEST(s.fired());
            };
            capy::run_async(ioc.get_executor())(write_body());
            ioc.run();
            BOOST_TEST(f.fired());
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
            if(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
            {
                BOOST_TEST(false);
                return;
            }
            make_native_adoptable(sv[1]);
            fault_scope f(sys::uring_sqe_full, 0);
            fault_scope g(sys::io_uring_submit_and_get_events, EBADF);
            io_context ioc(io_uring);
            local_stream_socket b(ioc);
            BOOST_TEST(!b.assign(sv[1]));
            std::error_code pec;
            auto wait_body = [&]() -> capy::task<>
            {
                auto [ec] = co_await b.wait(wait_type::read);
                pec = ec;
            };
            capy::run_async(ioc.get_executor())(wait_body());
            ioc.run();
            BOOST_TEST(f.fired());
            BOOST_TEST(g.fired());
            BOOST_TEST(pec == std::errc::resource_unavailable_try_again);
            BOOST_TEST(b.is_open());
            ::close(sv[0]);
        }
    }

    void testConnectCqeRewrite()
    {
        io_context ioc(io_uring);
        tcp_acceptor acc(ioc, uring_loopback());
        tcp_socket c(ioc);
        BOOST_TEST(!c.open(tcp::v4()));
        std::error_code cec;
        bool fired = false;
        {
            cqe_fault_scope q(
                c.native_handle(), IORING_OP_CONNECT, -ECONNREFUSED);
            auto connect_body = [&]() -> capy::task<>
            {
                auto [ec] = co_await c.connect(acc.local_endpoint());
                cec = ec;
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
        io_context ioc(io_uring);
        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(uring_loopback()));
        tcp_socket c(ioc), s(ioc);
        std::error_code aec;
        bool fired = false;
        {
            cqe_fault_scope q(
                acc.native_handle(), IORING_OP_ACCEPT, -EMFILE);
            BOOST_TEST(!acc.listen());
            auto accept_body = [&]() -> capy::task<>
            {
                auto [ec] = co_await acc.accept(s);
                aec = ec;
            };
            capy::run_async(ioc.get_executor())(accept_body());
            auto connect_body = [&]() -> capy::task<>
            {
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
        io_context ioc(io_uring);
        auto [a, b] = test::make_socket_pair(ioc);
        char buf[8] = "1234567";
        std::error_code sec, rec, pec, smec, rvec;
        bool sfired = false, rfired = false, pfired = false;
        bool smfired = false, rvfired = false;
        auto body = [&]() -> capy::task<>
        {
            {
                fault_scope f(sys::sendmsg, EAGAIN);
                cqe_fault_scope q(
                    a.native_handle(), IORING_OP_SEND, -EPIPE);
                auto [ec, n] = co_await a.write_some(
                    capy::const_buffer(buf, 7));
                std::ignore = n;
                sec = ec;
                BOOST_TEST(f.fired());
                sfired = q.fired();
            }
            {
                fault_scope f(sys::readv, EAGAIN);
                cqe_fault_scope q(
                    b.native_handle(), IORING_OP_RECV, -ECONNRESET);
                auto [ec, n] = co_await b.read_some(
                    capy::mutable_buffer(buf, 7));
                std::ignore = n;
                rec = ec;
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
                    capy::const_buffer(buf, 4),
                    capy::const_buffer(buf + 4, 3)};
                auto [ec, n] = co_await a.write_some(cb);
                std::ignore = n;
                smec = ec;
                smfired = q.fired();
            }
            {
                char x[4] = {}, y[4] = {};
                cqe_fault_scope q(
                    b.native_handle(), IORING_OP_READV, -EIO);
                std::array<capy::mutable_buffer, 2> mb{
                    capy::mutable_buffer(x, 4),
                    capy::mutable_buffer(y, 3)};
                auto [ec, n] = co_await b.read_some(mb);
                std::ignore = n;
                rvec = ec;
                rvfired = q.fired();
            }
            {
                // The rewritten RECV really ran, so b's buffer is empty
                // again; the poll needs data on the other side or it
                // would park forever instead of delivering a CQE.
                auto [ec, n] = co_await b.write_some(
                    capy::const_buffer(buf, 7));
                std::ignore = n;
                BOOST_TEST(!ec);
            }
            {
                cqe_fault_scope q(
                    a.native_handle(), IORING_OP_POLL_ADD, -EBADF);
                auto [ec] = co_await a.wait(wait_type::read);
                pec = ec;
                pfired = q.fired();
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
        io_context ioc(io_uring);
        udp_socket a(ioc), b(ioc);
        BOOST_TEST(!a.open(udp::v4()));
        BOOST_TEST(!b.open(udp::v4()));
        BOOST_TEST(!a.bind(uring_loopback()));
        BOOST_TEST(!b.bind(uring_loopback()));
        char buf[8] = "1234567";
        std::error_code sec, rec;
        bool sfired = false, rfired = false;
        auto body = [&]() -> capy::task<>
        {
            {
                fault_scope f(sys::sendmsg, EAGAIN);
                cqe_fault_scope q(
                    a.native_handle(), IORING_OP_SENDMSG, -EIO);
                auto [ec, n] = co_await a.send_to(
                    capy::const_buffer(buf, 7), b.local_endpoint());
                std::ignore = n;
                sec = ec;
                BOOST_TEST(f.fired());
                sfired = q.fired();
            }
            {
                fault_scope f(sys::recvmsg, EAGAIN);
                cqe_fault_scope q(
                    b.native_handle(), IORING_OP_RECVMSG, -EIO);
                endpoint from;
                auto [ec, n] = co_await b.recv_from(
                    capy::mutable_buffer(buf, 7), from);
                std::ignore = n;
                rec = ec;
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
        io_context ioc(io_uring);
        auto sf_path = temp_path("uring_sf");
        auto rf_path = temp_path("uring_raf");
        stream_file sf(ioc);
        random_access_file rf(ioc);
        BOOST_TEST(!sf.open(sf_path,
            file_base::read_write | file_base::create));
        BOOST_TEST(!rf.open(rf_path,
            file_base::read_write | file_base::create));
        char buf[8] = "1234567";
        std::error_code swec, srec, rwec, rrec;
        bool swf = false, srf = false, rwf = false, rrf = false;
        auto body = [&]() -> capy::task<>
        {
            {
                cqe_fault_scope q(
                    sf.native_handle(), IORING_OP_WRITEV, -EIO);
                auto [ec, n] = co_await sf.write_some(
                    capy::const_buffer(buf, 7));
                std::ignore = n;
                swec = ec;
                swf = q.fired();
            }
            {
                // A faulted completion is not sticky: the file still
                // takes writes, and this one gives the read below
                // something to find.
                auto [ec, n] = co_await sf.write_some(
                    capy::const_buffer(buf, 7));
                std::ignore = n;
                BOOST_TEST(!ec);
            }
            {
                auto [ec, pos] = sf.seek(0, file_base::seek_set);
                std::ignore = pos;
                BOOST_TEST(!ec);
            }
            {
                cqe_fault_scope q(
                    sf.native_handle(), IORING_OP_READV, -EIO);
                auto [ec, n] = co_await sf.read_some(
                    capy::mutable_buffer(buf, 7));
                std::ignore = n;
                srec = ec;
                srf = q.fired();
            }
            {
                cqe_fault_scope q(
                    rf.native_handle(), IORING_OP_WRITEV, -EIO);
                auto [ec, n] = co_await rf.write_some_at(
                    0, capy::const_buffer(buf, 7));
                std::ignore = n;
                rwec = ec;
                rwf = q.fired();
            }
            {
                // Same for the random-access file: the faulted
                // completion leaves it usable, and this write seeds the
                // read below.
                auto [ec, n] = co_await rf.write_some_at(
                    0, capy::const_buffer(buf, 7));
                std::ignore = n;
                BOOST_TEST(!ec);
            }
            {
                cqe_fault_scope q(
                    rf.native_handle(), IORING_OP_READV, -EIO);
                auto [ec, n] = co_await rf.read_some_at(
                    0, capy::mutable_buffer(buf, 7));
                std::ignore = n;
                rrec = ec;
                rrf = q.fired();
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
        io_context ioc(io_uring);
        // The listen inside the constructor is what arms the multishot
        // accept, and it finds the one SQE already spent on the wakeup
        // poll.
        tcp_acceptor acc(ioc, uring_loopback());
        tcp_socket s(ioc);
        std::error_code aec, wec;
        auto body = [&]() -> capy::task<>
        {
            {
                auto [ec] = co_await acc.accept(s);
                aec = ec;
            }
            {
                // Readiness on this backend means a future delivery,
                // which the failed arm has ruled out just as squarely.
                auto [ec] = co_await acc.wait(wait_type::read);
                wec = ec;
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
        auto accept_body = [&]() -> capy::task<>
        {
            auto [ec] = co_await acc.accept(s2);
            aec2 = ec;
        };
        auto client_body = [&]() -> capy::task<>
        {
            auto [ec] = co_await c.connect(acc.local_endpoint());
            cec = ec;
        };
        capy::run_async(ioc.get_executor())(accept_body());
        capy::run_async(ioc.get_executor())(client_body());
        ioc.run();
        BOOST_TEST(!cec);
        BOOST_TEST(!aec2);
        BOOST_TEST(s2.is_open());
    }

    void run()
    {
        if(skip_under_valgrind())
            return;
        testRingInitFails();
        testWaitFails();
        testAcceptorDrainSubmitFails();
        testAcceptorArmSqFull();
        testSqFull();
        testConnectCqeRewrite();
        testAcceptCqeRewrite();
        testStreamCqeRewrites();
        testDatagramCqeRewrites();
        testFileCqeRewrites();
    }
};

TEST_SUITE(uring_faults, "boost.corosio.fault.io_uring");

} // boost::corosio::test::fault

#endif
