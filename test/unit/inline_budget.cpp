//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Operations whose result is known synchronously normally resume the
// awaiting coroutine inline, rationed by a per-run budget. Once the
// budget is spent the completion must instead be deferred through the
// scheduler's completed-op queue. These tests pin the deferred path:
// suite A disables the reactor budget outright via io_context_options,
// suite B drains the io_uring budget with long chains of synchronously
// completing ops so the tail of each chain is forced through deferral.

#include <boost/corosio/detail/platform.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/udp.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/wait_type.hpp>

#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <stop_token>
#include <system_error>

#if BOOST_COROSIO_POSIX
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_socket.hpp>

#include <boost/corosio/test/temp_path.hpp>
#endif

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

namespace {

// Enough sequential sync-result ops to drain any budget configuration
// (the largest budget any scheduler hands out is 16 per dispatch).
[[maybe_unused]] constexpr int budget_cycles = 20;

} // namespace

template<auto Backend>
struct budget_disabled_test
{
    static io_context
    make_ioc()
    {
        io_context_options opts;
        opts.inline_budget_max = 0;
        return io_context(Backend, opts);
    }

    void
    testStreamSyncOpsDefer()
    {
        auto ioc = make_ioc();
        auto ex  = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        std::error_code wec;
        std::size_t wn = 0;
        auto writer = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.write_some(capy::const_buffer("abcd", 4));
            wec = ec;
            wn  = n;
        };
        capy::run_async(ex)(writer());
        ioc.run();
        ioc.restart();
        BOOST_TEST(!wec);
        BOOST_TEST_EQ(wn, 4u);

        char buf[16];
        std::error_code rec, wtec;
        std::size_t rn = 0;
        auto reader = [&]() -> capy::task<> {
            auto [wt_ec] = co_await s2.wait(wait_type::read);
            wtec         = wt_ec;
            auto [ec, n] =
                co_await s2.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            rec = ec;
            rn  = n;
        };
        capy::run_async(ex)(reader());
        ioc.run();
        BOOST_TEST(!wtec);
        BOOST_TEST(!rec);
        BOOST_TEST_EQ(rn, 4u);
    }

    void
    testDatagramSyncOpsDefer()
    {
        auto ioc = make_ioc();
        auto ex  = ioc.get_executor();

        udp_socket s1(ioc), s2(ioc);
        BOOST_TEST(!s1.open(udp::v4()));
        BOOST_TEST(!s2.open(udp::v4()));
        BOOST_TEST(!s1.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!s2.bind(endpoint(ipv4_address::loopback(), 0)));
        auto ep1 = s1.local_endpoint();
        auto ep2 = s2.local_endpoint();

        int ok = 0;
        auto sender = [&]() -> capy::task<> {
            // send_to before connect: BSD rejects an explicit
            // destination on a connected datagram socket.
            auto [tec, tn] = co_await s2.send_to(capy::const_buffer("y", 1), ep1);
            if (!tec && tn == 1)
                ++ok;
            auto [cec] = co_await s2.connect(ep1);
            if (!cec)
                ++ok;
            auto [ec, n] = co_await s2.send(capy::const_buffer("x", 1));
            if (!ec && n == 1)
                ++ok;
        };
        capy::run_async(ex)(sender());
        ioc.run();
        ioc.restart();
        BOOST_TEST_EQ(ok, 3);

        char buf[8];
        endpoint source;
        ok = 0;
        auto receiver = [&]() -> capy::task<> {
            auto [wec] = co_await s1.wait(wait_type::read);
            if (!wec)
                ++ok;
            auto [ec, n] = co_await s1.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            if (!ec && n == 1)
                ++ok;
            auto [ec2, n2] = co_await s1.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            if (!ec2 && n2 == 1)
                ++ok;
        };
        capy::run_async(ex)(receiver());
        ioc.run();
        BOOST_TEST_EQ(ok, 3);
        BOOST_TEST(source == ep2);
    }

#if BOOST_COROSIO_POSIX
    void
    testLocalSyncOpsDefer()
    {
        auto ioc = make_ioc();
        auto ex  = ioc.get_executor();

        local_stream_socket a(ioc), b(ioc);
        if (auto ec = connect_pair(a, b))
            throw std::system_error(ec, "connect_pair");
        local_datagram_socket da(ioc), db(ioc);
        if (auto ec = connect_pair(da, db))
            throw std::system_error(ec, "connect_pair");

        char buf[8];
        int ok = 0;
        auto driver = [&]() -> capy::task<> {
            auto [wec, wn] = co_await a.write_some(capy::const_buffer("hi", 2));
            if (!wec && wn == 2)
                ++ok;
            auto [rec, rn] =
                co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            if (!rec && rn == 2)
                ++ok;
            auto [sec, sn] = co_await da.send(capy::const_buffer("z", 1));
            if (!sec && sn == 1)
                ++ok;
            auto [dec, dn] =
                co_await db.recv(capy::mutable_buffer(buf, sizeof(buf)));
            if (!dec && dn == 1)
                ++ok;
        };
        capy::run_async(ex)(driver());
        ioc.run();
        BOOST_TEST_EQ(ok, 4);
    }
#endif

    void
    run()
    {
        testStreamSyncOpsDefer();
        testDatagramSyncOpsDefer();
#if BOOST_COROSIO_POSIX
        testLocalSyncOpsDefer();
#endif
    }
};

COROSIO_REACTOR_BACKEND_TESTS(budget_disabled_test, "boost.corosio.budget_disabled")

#if BOOST_COROSIO_HAS_IO_URING

struct io_uring_budget_test
{
    // Interleaved cycles: wherever the deferral boundary lands, every op
    // kind in the cycle crosses it at some iteration.

    void
    testStreamStoppedOpsDefer()
    {
        io_context ioc(io_uring);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);
        auto peer = s1.remote_endpoint();

        std::stop_source ss;
        ss.request_stop();

        char buf[8];
        int canceled = 0;
        auto driver  = [&]() -> capy::task<> {
            for (int i = 0; i < budget_cycles; ++i)
            {
                auto [rec, rn] = co_await s1.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                std::ignore = rn;
                if (rec == capy::cond::canceled)
                    ++canceled;
                auto [wec, wn] =
                    co_await s1.write_some(capy::const_buffer("x", 1));
                std::ignore = wn;
                if (wec == capy::cond::canceled)
                    ++canceled;
                auto [cec] = co_await s1.connect(peer);
                if (cec == capy::cond::canceled)
                    ++canceled;
            }
        };
        capy::run_async(ex, ss.get_token())(driver());
        ioc.run();
        BOOST_TEST_EQ(canceled, 3 * budget_cycles);
    }

    void
    testStreamSuccessOpsDefer()
    {
        io_context ioc(io_uring);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        char big[64] = {};
        std::error_code pec;
        auto preload = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s2.write_some(capy::const_buffer(big, sizeof(big)));
            std::ignore = n;
            pec         = ec;
        };
        capy::run_async(ex)(preload());
        ioc.run();
        ioc.restart();
        BOOST_TEST(!pec);

        char c;
        int ok      = 0;
        auto driver = [&]() -> capy::task<> {
            for (int i = 0; i < budget_cycles; ++i)
            {
                auto [rec, rn] =
                    co_await s1.read_some(capy::mutable_buffer(&c, 1));
                if (!rec && rn == 1)
                    ++ok;
                auto [wec, wn] =
                    co_await s1.write_some(capy::const_buffer("y", 1));
                if (!wec && wn == 1)
                    ++ok;
            }
        };
        capy::run_async(ex)(driver());
        ioc.run();
        BOOST_TEST_EQ(ok, 2 * budget_cycles);
    }

    void
    testUdpStoppedOpsDefer()
    {
        io_context ioc(io_uring);
        auto ex = ioc.get_executor();

        udp_socket s1(ioc), s2(ioc);
        BOOST_TEST(!s1.open(udp::v4()));
        BOOST_TEST(!s2.open(udp::v4()));
        BOOST_TEST(!s1.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!s2.bind(endpoint(ipv4_address::loopback(), 0)));
        auto ep2 = s2.local_endpoint();

        std::error_code cec;
        auto connecter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.connect(ep2);
            cec       = ec;
        };
        capy::run_async(ex)(connecter());
        ioc.run();
        ioc.restart();
        BOOST_TEST(!cec);

        std::stop_source ss;
        ss.request_stop();

        char buf[8];
        endpoint source;
        int canceled = 0;
        auto driver  = [&]() -> capy::task<> {
            for (int i = 0; i < budget_cycles; ++i)
            {
                auto [aec, an] =
                    co_await s1.send_to(capy::const_buffer("x", 1), ep2);
                std::ignore = an;
                if (aec == capy::cond::canceled)
                    ++canceled;
                auto [bec, bn] = co_await s1.recv_from(
                    capy::mutable_buffer(buf, sizeof(buf)), source);
                std::ignore = bn;
                if (bec == capy::cond::canceled)
                    ++canceled;
                auto [dec] = co_await s1.connect(ep2);
                if (dec == capy::cond::canceled)
                    ++canceled;
                auto [eec, en] = co_await s1.send(capy::const_buffer("x", 1));
                std::ignore    = en;
                if (eec == capy::cond::canceled)
                    ++canceled;
                auto [fec, fn] =
                    co_await s1.recv(capy::mutable_buffer(buf, sizeof(buf)));
                std::ignore = fn;
                if (fec == capy::cond::canceled)
                    ++canceled;
            }
        };
        capy::run_async(ex, ss.get_token())(driver());
        ioc.run();
        BOOST_TEST_EQ(canceled, 5 * budget_cycles);
    }

    void
    testUdpSuccessOpsDefer()
    {
        io_context ioc(io_uring);
        auto ex = ioc.get_executor();

        udp_socket s1(ioc), s2(ioc);
        BOOST_TEST(!s1.open(udp::v4()));
        BOOST_TEST(!s2.open(udp::v4()));
        BOOST_TEST(!s1.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!s2.bind(endpoint(ipv4_address::loopback(), 0)));
        auto ep1 = s1.local_endpoint();
        auto ep2 = s2.local_endpoint();

        BOOST_TEST_NO_THROW(
            s1.set_option(socket_option::receive_buffer_size(1 << 20)));

        int preloaded = 0;
        auto preload  = [&]() -> capy::task<> {
            auto [cec] = co_await s2.connect(ep1);
            std::ignore = cec;
            for (int i = 0; i < 5 * budget_cycles; ++i)
            {
                auto [ec, n] = co_await s2.send(capy::const_buffer("d", 1));
                if (!ec && n == 1)
                    ++preloaded;
            }
        };
        capy::run_async(ex)(preload());
        ioc.run();
        ioc.restart();
        BOOST_TEST_EQ(preloaded, 5 * budget_cycles);

        std::error_code cec;
        auto connecter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.connect(ep2);
            cec       = ec;
        };
        capy::run_async(ex)(connecter());
        ioc.run();
        ioc.restart();
        BOOST_TEST(!cec);

        // Consecutive runs of one op kind: whatever the budget window,
        // a run longer than the largest budget defers at least once.
        char buf[8];
        endpoint source;
        int ok      = 0;
        auto driver = [&]() -> capy::task<> {
            for (int i = 0; i < 2 * budget_cycles; ++i)
            {
                auto [ec, n] =
                    co_await s1.recv(capy::mutable_buffer(buf, sizeof(buf)));
                if (!ec && n == 1)
                    ++ok;
            }
            for (int i = 0; i < 2 * budget_cycles; ++i)
            {
                auto [ec, n] = co_await s1.recv_from(
                    capy::mutable_buffer(buf, sizeof(buf)), source);
                if (!ec && n == 1)
                    ++ok;
            }
            for (int i = 0; i < budget_cycles; ++i)
            {
                auto [ec, n] = co_await s1.send(capy::const_buffer("u", 1));
                if (!ec && n == 1)
                    ++ok;
            }
            for (int i = 0; i < budget_cycles; ++i)
            {
                auto [ec] = co_await s1.connect(ep2);
                if (!ec)
                    ++ok;
            }
        };
        capy::run_async(ex)(driver());
        ioc.run();
        BOOST_TEST_EQ(ok, 6 * budget_cycles);
        BOOST_TEST(source == ep2);
    }

#if BOOST_COROSIO_POSIX
    void
    testLocalStreamStoppedOpsDefer()
    {
        io_context ioc(io_uring);
        auto ex = ioc.get_executor();

        local_stream_socket a(ioc), b(ioc);
        if (auto ec = connect_pair(a, b))
            throw std::system_error(ec, "connect_pair");

        test::temp_socket_dir tmp;
        auto target = local_endpoint(tmp.path());

        std::stop_source ss;
        ss.request_stop();

        char buf[8];
        int canceled = 0;
        auto driver  = [&]() -> capy::task<> {
            for (int i = 0; i < budget_cycles; ++i)
            {
                auto [rec, rn] = co_await a.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                std::ignore = rn;
                if (rec == capy::cond::canceled)
                    ++canceled;
                auto [wec, wn] =
                    co_await a.write_some(capy::const_buffer("x", 1));
                std::ignore = wn;
                if (wec == capy::cond::canceled)
                    ++canceled;
                auto [cec] = co_await a.connect(target);
                if (cec == capy::cond::canceled)
                    ++canceled;
            }
        };
        capy::run_async(ex, ss.get_token())(driver());
        ioc.run();
        BOOST_TEST_EQ(canceled, 3 * budget_cycles);
    }

    void
    testLocalStreamSuccessOpsDefer()
    {
        io_context ioc(io_uring);
        auto ex = ioc.get_executor();

        local_stream_socket a(ioc), b(ioc);
        if (auto ec = connect_pair(a, b))
            throw std::system_error(ec, "connect_pair");

        char big[64] = {};
        std::error_code pec;
        auto preload = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await b.write_some(capy::const_buffer(big, sizeof(big)));
            std::ignore = n;
            pec         = ec;
        };
        capy::run_async(ex)(preload());
        ioc.run();
        ioc.restart();
        BOOST_TEST(!pec);

        char c;
        int ok      = 0;
        auto driver = [&]() -> capy::task<> {
            for (int i = 0; i < budget_cycles; ++i)
            {
                auto [rec, rn] =
                    co_await a.read_some(capy::mutable_buffer(&c, 1));
                if (!rec && rn == 1)
                    ++ok;
                auto [wec, wn] =
                    co_await a.write_some(capy::const_buffer("y", 1));
                if (!wec && wn == 1)
                    ++ok;
            }
        };
        capy::run_async(ex)(driver());
        ioc.run();
        BOOST_TEST_EQ(ok, 2 * budget_cycles);
    }

    void
    testLocalDatagramStoppedOpsDefer()
    {
        io_context ioc(io_uring);
        auto ex = ioc.get_executor();

        test::temp_socket_dir tmp1;
        test::temp_socket_dir tmp2;
        local_datagram_socket s1(ioc), s2(ioc);
        BOOST_TEST(!s1.open());
        BOOST_TEST(!s2.open());
        BOOST_TEST(!s1.bind(local_endpoint(tmp1.path())));
        BOOST_TEST(!s2.bind(local_endpoint(tmp2.path())));
        auto ep2 = local_endpoint(tmp2.path());

        std::error_code cec;
        auto connecter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.connect(ep2);
            cec       = ec;
        };
        capy::run_async(ex)(connecter());
        ioc.run();
        ioc.restart();
        BOOST_TEST(!cec);

        std::stop_source ss;
        ss.request_stop();

        char buf[8];
        local_endpoint source;
        int canceled = 0;
        auto driver  = [&]() -> capy::task<> {
            for (int i = 0; i < budget_cycles; ++i)
            {
                auto [aec, an] = co_await s1.send(capy::const_buffer("x", 1));
                std::ignore    = an;
                if (aec == capy::cond::canceled)
                    ++canceled;
                auto [bec, bn] =
                    co_await s1.recv(capy::mutable_buffer(buf, sizeof(buf)));
                std::ignore = bn;
                if (bec == capy::cond::canceled)
                    ++canceled;
                auto [dec] = co_await s1.connect(ep2);
                if (dec == capy::cond::canceled)
                    ++canceled;
                auto [eec, en] =
                    co_await s1.send_to(capy::const_buffer("x", 1), ep2);
                std::ignore = en;
                if (eec == capy::cond::canceled)
                    ++canceled;
                auto [fec, fn] = co_await s1.recv_from(
                    capy::mutable_buffer(buf, sizeof(buf)), source);
                std::ignore = fn;
                if (fec == capy::cond::canceled)
                    ++canceled;
            }
        };
        capy::run_async(ex, ss.get_token())(driver());
        ioc.run();
        BOOST_TEST_EQ(canceled, 5 * budget_cycles);
    }

    void
    testLocalDatagramSuccessOpsDefer()
    {
        io_context ioc(io_uring);
        auto ex = ioc.get_executor();

        test::temp_socket_dir tmp1;
        test::temp_socket_dir tmp2;
        local_datagram_socket s1(ioc), s2(ioc);
        BOOST_TEST(!s1.open());
        BOOST_TEST(!s2.open());
        BOOST_TEST(!s1.bind(local_endpoint(tmp1.path())));
        BOOST_TEST(!s2.bind(local_endpoint(tmp2.path())));
        auto ep1 = local_endpoint(tmp1.path());
        auto ep2 = local_endpoint(tmp2.path());

        // A datagram socket's queue depth is bounded by the unix
        // datagram limit, not SO_RCVBUF, so only a few can be held
        // unread. Spend the inline budget on sends and connects first;
        // every recv that follows in the same frame then takes the
        // deferred path even though only a handful are buffered.
        int const held = 6;
        int preloaded  = 0;
        auto preload   = [&]() -> capy::task<> {
            for (int i = 0; i < held; ++i)
            {
                auto [ec, n] =
                    co_await s2.send_to(capy::const_buffer("d", 1), ep1);
                if (!ec && n == 1)
                    ++preloaded;
            }
        };
        capy::run_async(ex)(preload());
        ioc.run();
        ioc.restart();
        BOOST_TEST_EQ(preloaded, held);

        char buf[8];
        local_endpoint source;
        int ok      = 0;
        auto driver = [&]() -> capy::task<> {
            // Spend the budget with connects: a datagram connect is a
            // local re-point, always inline, and queues nothing (a
            // send_to run would overflow the unconsumed peer queue and
            // start failing with EAGAIN).
            for (int i = 0; i < budget_cycles; ++i)
            {
                auto [ec] = co_await s1.connect(ep2);
                if (!ec)
                    ++ok;
            }
            // Budget spent: a few send_to, kept under the queue limit,
            // take the deferred send success arm.
            for (int i = 0; i < held; ++i)
            {
                auto [ec, n] =
                    co_await s1.send_to(capy::const_buffer("u", 1), ep2);
                if (!ec && n == 1)
                    ++ok;
            }
            // The preloaded datagrams complete through the deferred
            // recv / recv_from success arm.
            for (int i = 0; i < held / 2; ++i)
            {
                auto [ec, n] =
                    co_await s1.recv(capy::mutable_buffer(buf, sizeof(buf)));
                if (!ec && n == 1)
                    ++ok;
            }
            for (int i = 0; i < held / 2; ++i)
            {
                auto [ec, n] = co_await s1.recv_from(
                    capy::mutable_buffer(buf, sizeof(buf)), source);
                if (!ec && n == 1)
                    ++ok;
            }
        };
        capy::run_async(ex)(driver());
        ioc.run();
        BOOST_TEST_EQ(ok, budget_cycles + 2 * held);
    }
#endif // BOOST_COROSIO_POSIX

    void
    run()
    {
        testStreamStoppedOpsDefer();
        testStreamSuccessOpsDefer();
        testUdpStoppedOpsDefer();
        testUdpSuccessOpsDefer();
#if BOOST_COROSIO_POSIX
        testLocalStreamStoppedOpsDefer();
        testLocalStreamSuccessOpsDefer();
        testLocalDatagramStoppedOpsDefer();
        testLocalDatagramSuccessOpsDefer();
#endif
    }
};

TEST_SUITE(io_uring_budget_test, "boost.corosio.io_uring_budget");

#endif // BOOST_COROSIO_HAS_IO_URING

} // namespace boost::corosio
