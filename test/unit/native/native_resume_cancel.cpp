//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// The native awaitables re-check the stop token at resume time and
// report cancellation even when the operation itself completed. No
// other suite drives the native fronts with a stopped token, so these
// resume-time arms are exercised here: once with a pre-stopped token,
// and once with the stop requested after data is already buffered so
// the op genuinely succeeds before the resume-time check overrides it.

#include <boost/corosio/detail/platform.hpp>

#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/native/native_random_access_file.hpp>
#include <boost/corosio/native/native_stream_file.hpp>
#include <boost/corosio/native/native_tcp_acceptor.hpp>
#include <boost/corosio/native/native_tcp_socket.hpp>
#include <boost/corosio/native/native_udp_socket.hpp>

#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <filesystem>
#include <fstream>
#include <stop_token>
#include <system_error>

#if BOOST_COROSIO_POSIX
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/native/native_local_datagram_socket.hpp>
#include <boost/corosio/native/native_local_stream_acceptor.hpp>
#include <boost/corosio/native/native_local_stream_socket.hpp>

#include <boost/corosio/test/temp_path.hpp>
#endif

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_resume_cancel_test
{
    void testTcpPreStopped()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] = test::make_socket_pair<
            native_tcp_socket<Backend>,
            native_tcp_acceptor<Backend>>(ioc);
        auto peer = s1.remote_endpoint();

        std::stop_source ss;
        ss.request_stop();

        char buf[8];
        int canceled = 0;
        auto driver  = [&]() -> capy::task<> {
            auto [rec, rn] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            if (rec == capy::cond::canceled && rn == 0)
                ++canceled;
            auto [wec, wn] = co_await s1.write_some(capy::const_buffer("x", 1));
            if (wec == capy::cond::canceled && wn == 0)
                ++canceled;
            auto [cec] = co_await s1.connect(peer);
            if (cec == capy::cond::canceled)
                ++canceled;
            auto [tec] = co_await s1.wait(wait_type::read);
            if (tec == capy::cond::canceled)
                ++canceled;
        };
        capy::run_async(ex, ss.get_token())(driver());
        ioc.run();
        BOOST_TEST_EQ(canceled, 4);
    }

    void testTcpStopAfterDataBuffered()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] = test::make_socket_pair<
            native_tcp_socket<Backend>,
            native_tcp_acceptor<Backend>>(ioc);

        std::error_code pec;
        auto preload = [&]() -> capy::task<> {
            auto [ec, n] = co_await s2.write_some(capy::const_buffer("hi", 2));
            std::ignore  = n;
            pec          = ec;
        };
        capy::run_async(ex)(preload());
        ioc.run();
        ioc.restart();
        BOOST_TEST(!pec);

        std::stop_source ss;
        char buf[8];
        std::error_code rec;
        std::size_t rn = 99;
        auto reader    = [&]() -> capy::task<> {
            ss.request_stop();
            auto [ec, n] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            rec = ec;
            rn  = n;
        };
        capy::run_async(ex, ss.get_token())(reader());
        ioc.run();

        BOOST_TEST(rec == capy::cond::canceled);
        BOOST_TEST_EQ(rn, 0u);
    }

    void testTcpAcceptorPreStopped()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        native_tcp_acceptor<Backend> acc(ioc);
        BOOST_TEST(!acc.open(tcp::v4()));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());

        std::stop_source ss;
        ss.request_stop();

        native_tcp_socket<Backend> peer(ioc);
        int canceled = 0;
        auto driver  = [&]() -> capy::task<> {
            auto [aec] = co_await acc.accept(peer);
            if (aec == capy::cond::canceled)
                ++canceled;
            auto [sec, sock] = co_await acc.accept();
            if (sec == capy::cond::canceled)
                ++canceled;
            auto [wec] = co_await acc.wait(wait_type::read);
            if (wec == capy::cond::canceled)
                ++canceled;
        };
        capy::run_async(ex, ss.get_token())(driver());
        ioc.run();
        BOOST_TEST_EQ(canceled, 3);
        BOOST_TEST(!peer.is_open());
    }

    void testUdpPreStopped()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        native_udp_socket<Backend> s1(ioc), s2(ioc);
        BOOST_TEST(!s1.open(udp::v4()));
        BOOST_TEST(!s2.open(udp::v4()));
        BOOST_TEST(!s1.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!s2.bind(endpoint(ipv4_address::loopback(), 0)));
        auto peer = s2.local_endpoint();

        std::error_code cec0;
        auto connecter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.connect(peer);
            cec0      = ec;
        };
        capy::run_async(ex)(connecter());
        ioc.run();
        ioc.restart();
        BOOST_TEST(!cec0);

        std::stop_source ss;
        ss.request_stop();

        char buf[8];
        endpoint source;
        int canceled = 0;
        auto driver  = [&]() -> capy::task<> {
            auto [aec, an] = co_await s1.send(capy::const_buffer("x", 1));
            if (aec == capy::cond::canceled && an == 0)
                ++canceled;
            auto [bec, bn] =
                co_await s1.recv(capy::mutable_buffer(buf, sizeof(buf)));
            if (bec == capy::cond::canceled && bn == 0)
                ++canceled;
            auto [dec, dn] =
                co_await s1.send_to(capy::const_buffer("x", 1), peer);
            if (dec == capy::cond::canceled && dn == 0)
                ++canceled;
            auto [eec, en] = co_await s1.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            if (eec == capy::cond::canceled && en == 0)
                ++canceled;
            auto [fec] = co_await s1.connect(peer);
            if (fec == capy::cond::canceled)
                ++canceled;
            auto [gec] = co_await s1.wait(wait_type::read);
            if (gec == capy::cond::canceled)
                ++canceled;
        };
        capy::run_async(ex, ss.get_token())(driver());
        ioc.run();
        BOOST_TEST_EQ(canceled, 6);
    }

    // The file awaitables carry the same resume-time stop check as the
    // sockets; no other suite drives a file op with a stopped token.
    void testFileResumeCancel()
    {
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        auto rp   = std::filesystem::temp_directory_path() / "corosio_rc_r.tmp";
        auto wp   = std::filesystem::temp_directory_path() / "corosio_rc_w.tmp";
        { std::ofstream(rp) << "hello"; }
        { std::ofstream w(wp); }

        native_stream_file<Backend> sfr(ioc), sfw(ioc);
        native_random_access_file<Backend> rfr(ioc), rfw(ioc);
        BOOST_TEST(!sfr.open(rp.string(), file_base::read_only));
        BOOST_TEST(!sfw.open(wp.string(), file_base::write_only));
        BOOST_TEST(!rfr.open(rp.string(), file_base::read_only));
        BOOST_TEST(!rfw.open(wp.string(), file_base::write_only));

        std::stop_source ss;
        ss.request_stop();
        char buf[8];
        int canceled = 0;
        auto driver  = [&]() -> capy::task<> {
            auto [a, an] =
                co_await sfr.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            if (a == capy::cond::canceled && an == 0)
                ++canceled;
            auto [b, bn] = co_await sfw.write_some(capy::const_buffer("x", 1));
            if (b == capy::cond::canceled && bn == 0)
                ++canceled;
            auto [c, cn] = co_await rfr.read_some_at(
                0, capy::mutable_buffer(buf, sizeof(buf)));
            if (c == capy::cond::canceled && cn == 0)
                ++canceled;
            auto [d, dn] =
                co_await rfw.write_some_at(0, capy::const_buffer("x", 1));
            if (d == capy::cond::canceled && dn == 0)
                ++canceled;
        };
        capy::run_async(ex, ss.get_token())(driver());
        ioc.run();
        BOOST_TEST_EQ(canceled, 4);
        std::error_code rm;
        std::filesystem::remove(rp, rm);
        std::filesystem::remove(wp, rm);
    }

#if BOOST_COROSIO_POSIX
    void testLocalStreamPreStopped()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        native_local_stream_socket<Backend> a(ioc), b(ioc);
        if (auto ec = connect_pair(a, b))
            throw std::system_error(ec, "connect_pair");

        test::temp_socket_dir tmp;
        auto target = local_endpoint(tmp.path());

        std::stop_source ss;
        ss.request_stop();

        char buf[8];
        int canceled = 0;
        auto driver  = [&]() -> capy::task<> {
            auto [rec, rn] =
                co_await a.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            if (rec == capy::cond::canceled && rn == 0)
                ++canceled;
            auto [wec, wn] = co_await a.write_some(capy::const_buffer("x", 1));
            if (wec == capy::cond::canceled && wn == 0)
                ++canceled;
            auto [cec] = co_await a.connect(target);
            if (cec == capy::cond::canceled)
                ++canceled;
            auto [tec] = co_await a.wait(wait_type::read);
            if (tec == capy::cond::canceled)
                ++canceled;
        };
        capy::run_async(ex, ss.get_token())(driver());
        ioc.run();
        BOOST_TEST_EQ(canceled, 4);
    }

    void testLocalStreamAcceptorPreStopped()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        test::temp_socket_dir tmp;
        native_local_stream_acceptor<Backend> acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(local_endpoint(tmp.path())));
        BOOST_TEST(!acc.listen());

        std::stop_source ss;
        ss.request_stop();

        native_local_stream_socket<Backend> peer(ioc);
        int canceled = 0;
        auto driver  = [&]() -> capy::task<> {
            auto [aec] = co_await acc.accept(peer);
            if (aec == capy::cond::canceled)
                ++canceled;
            auto [sec, sock] = co_await acc.accept();
            if (sec == capy::cond::canceled)
                ++canceled;
            auto [wec] = co_await acc.wait(wait_type::read);
            if (wec == capy::cond::canceled)
                ++canceled;
        };
        capy::run_async(ex, ss.get_token())(driver());
        ioc.run();
        BOOST_TEST_EQ(canceled, 3);
        BOOST_TEST(!peer.is_open());
    }

    void testLocalDatagramPreStopped()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        test::temp_socket_dir tmp1;
        test::temp_socket_dir tmp2;
        native_local_datagram_socket<Backend> s1(ioc), s2(ioc);
        BOOST_TEST(!s1.open());
        BOOST_TEST(!s2.open());
        BOOST_TEST(!s1.bind(local_endpoint(tmp1.path())));
        BOOST_TEST(!s2.bind(local_endpoint(tmp2.path())));
        auto peer = local_endpoint(tmp2.path());

        std::error_code cec0;
        auto connecter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.connect(peer);
            cec0      = ec;
        };
        capy::run_async(ex)(connecter());
        ioc.run();
        ioc.restart();
        BOOST_TEST(!cec0);

        std::stop_source ss;
        ss.request_stop();

        char buf[8];
        local_endpoint source;
        int canceled = 0;
        auto driver  = [&]() -> capy::task<> {
            auto [aec, an] = co_await s1.send(capy::const_buffer("x", 1));
            if (aec == capy::cond::canceled && an == 0)
                ++canceled;
            auto [bec, bn] =
                co_await s1.recv(capy::mutable_buffer(buf, sizeof(buf)));
            if (bec == capy::cond::canceled && bn == 0)
                ++canceled;
            auto [dec, dn] =
                co_await s1.send_to(capy::const_buffer("x", 1), peer);
            if (dec == capy::cond::canceled && dn == 0)
                ++canceled;
            auto [eec, en] = co_await s1.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            if (eec == capy::cond::canceled && en == 0)
                ++canceled;
            auto [fec] = co_await s1.connect(peer);
            if (fec == capy::cond::canceled)
                ++canceled;
            auto [gec] = co_await s1.wait(wait_type::read);
            if (gec == capy::cond::canceled)
                ++canceled;
        };
        capy::run_async(ex, ss.get_token())(driver());
        ioc.run();
        BOOST_TEST_EQ(canceled, 6);
    }
#endif // BOOST_COROSIO_POSIX

    void run()
    {
        testTcpPreStopped();
        testTcpStopAfterDataBuffered();
        testTcpAcceptorPreStopped();
        testUdpPreStopped();
        testFileResumeCancel();
#if BOOST_COROSIO_POSIX
        testLocalStreamPreStopped();
        testLocalStreamAcceptorPreStopped();
        testLocalDatagramPreStopped();
#endif
    }
};

COROSIO_BACKEND_TESTS(native_resume_cancel_test, "boost.corosio.native.resume_cancel")

} // namespace boost::corosio
