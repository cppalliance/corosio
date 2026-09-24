//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// A stop request that races an operation's completion must not change
// what the operation reports. The stream contracts require that a
// completed transfer is reported verbatim — byte counts are never
// discarded — while a stop that arrives before initiation short-circuits
// with `canceled` and performs no I/O at all. The next operation on the
// still-stopped token reports the cancellation.

#include <boost/corosio/detail/platform.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/wait_type.hpp>

#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstring>
#include <stop_token>
#include <system_error>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct cancel_race_test
{
    // Zero inline budget defers synchronously-known completions through
    // the scheduler queue, so a stop request posted after initiation is
    // seen before the completed op resumes its awaiter.
    static io_context make_deferring_ioc()
    {
        io_context_options opts;
        opts.inline_budget_max = 0;
        return io_context(Backend, opts);
    }

    void testWriteCompletedThenStop()
    {
        auto ioc = make_deferring_ioc();
        auto ex  = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        std::stop_source ss;
        std::error_code wec = capy::error::eof;
        std::size_t wn      = 0;
        bool done           = false;

        auto writer = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.write_some(capy::const_buffer("hello", 5));
            wec  = ec;
            wn   = n;
            done = true;
        };
        auto stopper = [&]() -> capy::task<> {
            ss.request_stop();
            co_return;
        };

        capy::run_async(ex, ss.get_token())(writer());
        capy::run_async(ex)(stopper());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(!wec);
        BOOST_TEST_EQ(wn, 5u);
    }

    void testReadCompletedThenStop()
    {
        auto ioc = make_deferring_ioc();
        auto ex  = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        // Prime s2's receive buffer so the read completes at initiation.
        bool primed = false;
        auto primer = [&]() -> capy::task<> {
            auto [pwec, pwn] =
                co_await s1.write_some(capy::const_buffer("hello", 5));
            BOOST_TEST(!pwec);
            BOOST_TEST_EQ(pwn, 5u);
            auto [wtec] = co_await s2.wait(wait_type::read);
            BOOST_TEST(!wtec);
            primed = true;
        };
        capy::run_async(ex)(primer());
        ioc.run();
        ioc.restart();
        BOOST_TEST(primed);

        std::stop_source ss;
        std::error_code rec = capy::error::eof;
        std::size_t rn      = 0;
        bool done           = false;
        char buf[8]         = {};

        auto reader = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s2.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            rec  = ec;
            rn   = n;
            done = true;
        };
        auto stopper = [&]() -> capy::task<> {
            ss.request_stop();
            co_return;
        };

        capy::run_async(ex, ss.get_token())(reader());
        capy::run_async(ex)(stopper());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(!rec);
        BOOST_TEST_EQ(rn, 5u);
        BOOST_TEST(std::memcmp(buf, "hello", 5) == 0);
    }

    void testPreStoppedWritePerformsNoIo()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        std::stop_source ss;
        ss.request_stop();

        std::error_code wec;
        std::size_t wn = 99;
        bool done      = false;

        auto writer = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.write_some(capy::const_buffer("XX", 2));
            wec          = ec;
            wn           = n;
            done         = true;
        };
        capy::run_async(ex, ss.get_token())(writer());
        ioc.run();
        ioc.restart();

        BOOST_TEST(done);
        BOOST_TEST(wec == capy::cond::canceled);
        BOOST_TEST_EQ(wn, 0u);

        // The cancelled write must not have reached the wire: the peer
        // sees only the follow-up bytes.
        std::error_code rec = capy::error::eof;
        std::size_t rn      = 0;
        char buf[8]         = {};
        bool verified       = false;

        auto verifier = [&]() -> capy::task<> {
            auto [vwec, vwn] =
                co_await s1.write_some(capy::const_buffer("yy", 2));
            BOOST_TEST(!vwec);
            BOOST_TEST_EQ(vwn, 2u);
            auto [ec, n] =
                co_await s2.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            rec      = ec;
            rn       = n;
            verified = true;
        };
        capy::run_async(ex)(verifier());
        ioc.run();

        BOOST_TEST(verified);
        BOOST_TEST(!rec);
        BOOST_TEST_EQ(rn, 2u);
        BOOST_TEST(std::memcmp(buf, "yy", 2) == 0);
    }

    void testPreStoppedReadKeepsData()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        // Prime s2's receive buffer so a speculative read would find data.
        bool primed = false;
        auto primer = [&]() -> capy::task<> {
            auto [pwec, pwn] =
                co_await s1.write_some(capy::const_buffer("zz", 2));
            BOOST_TEST(!pwec);
            BOOST_TEST_EQ(pwn, 2u);
            auto [wtec] = co_await s2.wait(wait_type::read);
            BOOST_TEST(!wtec);
            primed = true;
        };
        capy::run_async(ex)(primer());
        ioc.run();
        ioc.restart();
        BOOST_TEST(primed);

        std::stop_source ss;
        ss.request_stop();

        std::error_code cec;
        std::size_t cn = 99;
        bool done      = false;
        char cbuf[8]   = {};

        auto reader = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s2.read_some(capy::mutable_buffer(cbuf, sizeof(cbuf)));
            cec  = ec;
            cn   = n;
            done = true;
        };
        capy::run_async(ex, ss.get_token())(reader());
        ioc.run();
        ioc.restart();

        BOOST_TEST(done);
        BOOST_TEST(cec == capy::cond::canceled);
        BOOST_TEST_EQ(cn, 0u);

        // The cancelled read must not have consumed the buffered data.
        // Close the write side first so a data-eating implementation
        // reports EOF here instead of parking forever.
        s1.close();

        std::error_code rec = capy::error::eof;
        std::size_t rn      = 0;
        char buf[8]         = {};
        bool verified       = false;

        auto verifier = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s2.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            rec      = ec;
            rn       = n;
            verified = true;
        };
        capy::run_async(ex)(verifier());
        ioc.run();

        BOOST_TEST(verified);
        BOOST_TEST(!rec);
        BOOST_TEST_EQ(rn, 2u);
        BOOST_TEST(std::memcmp(buf, "zz", 2) == 0);
    }

    void run()
    {
        testWriteCompletedThenStop();
        testReadCompletedThenStop();
        testPreStoppedWritePerformsNoIo();
        testPreStoppedReadKeepsData();
    }
};

COROSIO_BACKEND_TESTS(cancel_race_test, "boost.corosio.cancel_race")

// A zero-transfer failure whose op also saw a cancellation request
// reports canceled: the caller asked for the stop, and the flag is
// what normalizes locally-induced completion errors (e.g. a close
// tearing down a pending op) across backends. The closed-object
// error path is the deterministic stand-in: the EBADF completion is
// deferred, and the stop lands before it is decoded.
//
// Excludes uring, where the closed-object error surfaces from the
// speculative syscall and completes inline before the stop exists —
// a completed error reported verbatim, which is equally conforming
// but a different interleaving.
template<auto Backend>
struct cancel_race_flag_test
{
    void run()
    {
        io_context_options opts;
        opts.inline_budget_max = 0;
        io_context ioc(Backend, opts);
        auto ex = ioc.get_executor();

        tcp_socket s1(ioc);
        std::stop_source ss;
        std::error_code wec;
        std::size_t wn = 99;
        bool done      = false;

        auto writer = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.write_some(capy::const_buffer("x", 1));
            wec          = ec;
            wn           = n;
            done         = true;
        };
        auto stopper = [&]() -> capy::task<> {
            ss.request_stop();
            co_return;
        };
        capy::run_async(ex, ss.get_token())(writer());
        capy::run_async(ex)(stopper());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(wec == capy::cond::canceled);
        BOOST_TEST_EQ(wn, 0u);
    }
};

COROSIO_REACTOR_BACKEND_TESTS(
    cancel_race_flag_test, "boost.corosio.cancel_race_flag")
COROSIO_TEST_IOCP_(cancel_race_flag_test, "boost.corosio.cancel_race_flag")

} // namespace boost::corosio
