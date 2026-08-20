//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/delay.hpp>
#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <array>
#include <system_error>

#include "context.hpp"
#include "test_suite.hpp"

/*
    Cross-backend error-condition consistency audit (issue #304).

    The remote/connection error family (peer RST, refused connect, write to a
    dead peer) is the class whose native mapping diverges per platform, so each
    scenario here is exercised under *every* backend via COROSIO_BACKEND_TESTS
    and asserts a portable capy::cond / std::errc condition. The contract:

        FIN  (graceful close) -> cond::eof
        RST  (hard close)     -> errc::connection_reset   (NOT cond::canceled)
        connect refused       -> errc::connection_refused
        write to dead peer    -> errc::connection_reset | broken_pipe

    #304: on Windows IOCP a remote RST used to surface as cond::canceled,
    conflating remote termination with local cancellation. cond::canceled must
    mean only local, deliberate cancellation (stop token / cancel() / close()) --
    that family is already covered across all backends elsewhere (tcp_socket,
    tcp_acceptor, connect, udp_socket, precancel, datagram_paths) and is not
    duplicated here.

    Not covered (intentional): accept of an aborted connection ->
    connection_aborted. It cannot be forced deterministically across platforms
    (the client must RST between the SYN and accept completing), so no hard test
    exists for it; the IOCP accept path maps it in win_*_acceptor_service.
*/

namespace boost::corosio {

template<auto Backend>
struct error_conditions_test
{
    // FIN: a graceful peer close surfaces as end-of-file. Anchor that contrasts
    // with the RST case below -- #304 is about keeping the two distinct.
    void testReadFinReturnsEof()
    {
        io_context ioc(Backend);
        // Linger=false => graceful FIN on close.
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        std::error_code read_ec;
        bool read_done = false;

        auto reader = [&](tcp_socket& b) -> capy::task<> {
            char buf[32] = {};
            auto [ec, n] =
                co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            read_ec   = ec;
            read_done = true;
            (void)n;
        };
        auto closer = [](tcp_socket& a) -> capy::task<> {
            a.close(); // graceful FIN
            co_return;
        };

        capy::run_async(ioc.get_executor())(reader(s2));
        capy::run_async(ioc.get_executor())(closer(s1));
        ioc.run();

        BOOST_TEST(read_done);
        BOOST_TEST(read_ec == capy::cond::eof);

        s1.close();
        s2.close();
    }

    // RST: a remote hard close surfaces as connection_reset -- never canceled
    // (the #304 regression) and never eof.
    void testReadResetReturnsConnectionReset()
    {
        io_context ioc(Backend);
        // Linger=true (default) => SO_LINGER{on,0}; close() aborts with a RST.
        auto [s1, s2] = test::make_socket_pair(ioc);

        std::error_code read_ec;
        bool read_done = false;

        // Reader parks with no data, so the RST lands on a pending op -- the
        // exact IOCP scenario that yields ERROR_NETNAME_DELETED.
        auto reader = [&](tcp_socket& b) -> capy::task<> {
            char buf[32] = {};
            auto [ec, n] =
                co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            read_ec   = ec;
            read_done = true;
            (void)n;
        };
        auto closer = [](tcp_socket& a) -> capy::task<> {
            a.close(); // RST via SO_LINGER{on,0}
            co_return;
        };

        capy::run_async(ioc.get_executor())(reader(s2));
        capy::run_async(ioc.get_executor())(closer(s1));
        ioc.run();

        BOOST_TEST(read_done);
        BOOST_TEST(read_ec == std::errc::connection_reset);
        BOOST_TEST(read_ec != capy::cond::canceled);
        BOOST_TEST(read_ec != capy::cond::eof);

        s1.close();
        s2.close();
    }

    // Writing to a peer that has died surfaces as connection_reset or
    // broken_pipe (which one depends on OS/timing), never canceled.
    void testWriteToResetPeerReturnsResetOrBrokenPipe()
    {
        io_context ioc(Backend);
        // Linger=true (default): the closed peer sends a RST.
        auto [s1, s2] = test::make_socket_pair(ioc);

        std::error_code write_ec;
        bool write_failed = false;

        auto task = [&](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            b.close(); // peer dies (RST)

            // Let the RST propagate before writing.
            (void)co_await corosio::delay(std::chrono::milliseconds(50));

            // Keep writing until the failure surfaces. The budget (256 x 64 KiB
            // = 16 MiB) is far beyond any platform's send buffer + in-flight
            // window (macOS buffers larger than Linux), so a dead peer must
            // error well within it; if it somehow does not, write_failed stays
            // false and the assertion below reports that directly.
            std::array<char, 65536> buf{};
            for (int i = 0; i < 256; ++i)
            {
                auto [ec, n] = co_await a.write_some(
                    capy::const_buffer(buf.data(), buf.size()));
                write_ec = ec;
                (void)n;
                if (ec)
                {
                    write_failed = true;
                    break;
                }
            }
        };

        capy::run_async(ioc.get_executor())(task(s1, s2));
        ioc.run();

        BOOST_TEST(write_failed);
        BOOST_TEST(
            write_ec == std::errc::connection_reset ||
            write_ec == std::errc::broken_pipe);
        BOOST_TEST(write_ec != capy::cond::canceled);

        s1.close();
        s2.close();
    }

    // Connecting to a closed port surfaces as connection_refused, never canceled.
    void testConnectRefusedReturnsConnectionRefused()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        BOOST_TEST(!sock.open());

        std::error_code connect_ec;

        auto task = [&]() -> capy::task<> {
            // Port 1 has no listener on loopback => refused.
            auto [ec] =
                co_await sock.connect(endpoint(ipv4_address::loopback(), 1));
            connect_ec = ec;
        };

        capy::run_async(ioc.get_executor())(task());
        ioc.run();

        BOOST_TEST(connect_ec == std::errc::connection_refused);
        BOOST_TEST(connect_ec != capy::cond::canceled);

        sock.close();
    }

    void run()
    {
        testReadFinReturnsEof();
        testReadResetReturnsConnectionReset();
        testWriteToResetPeerReturnsResetOrBrokenPipe();
        testConnectRefusedReturnsConnectionRefused();
    }
};

COROSIO_BACKEND_TESTS(error_conditions_test, "boost.corosio.error_conditions")

} // namespace boost::corosio
