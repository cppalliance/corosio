//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/local_stream_socket.hpp>

#include <boost/corosio/delay.hpp>
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/test/temp_path.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>
#include <boost/capy/concept/read_stream.hpp>
#include <boost/capy/concept/write_stream.hpp>

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#else
#include <boost/corosio/native/detail/iocp/win_windows.hpp>
#endif

#include <stop_token>

#include <chrono>
#include <compare>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

// Verify local_stream_socket satisfies stream concepts

static_assert(capy::ReadStream<local_stream_socket>);
static_assert(capy::WriteStream<local_stream_socket>);

template<auto Backend>
struct local_stream_socket_test
{
    void testConstruction()
    {
        io_context ioc(Backend);
        local_stream_socket sock(ioc);
        BOOST_TEST_EQ(sock.is_open(), false);
    }

    void testOpen()
    {
        io_context ioc(Backend);
        local_stream_socket sock(ioc);

        sock.open();
        BOOST_TEST_EQ(sock.is_open(), true);

        sock.close();
        BOOST_TEST_EQ(sock.is_open(), false);
    }

    void testMove()
    {
        io_context ioc(Backend);
        local_stream_socket s1(ioc);
        s1.open();
        BOOST_TEST_EQ(s1.is_open(), true);

        local_stream_socket s2(std::move(s1));
        BOOST_TEST_EQ(s2.is_open(), true);
        BOOST_TEST_EQ(s1.is_open(), false);
    }

    void testConnectAccept()
    {
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        local_stream_acceptor acc(ioc);
        acc.open();
        auto ec = acc.bind(local_endpoint(path));
        BOOST_TEST_EQ(!ec, true);
        ec = acc.listen();
        BOOST_TEST_EQ(!ec, true);

        std::error_code accept_ec, connect_ec;
        bool accept_done = false, connect_done = false;

        local_stream_socket server(ioc);
        local_stream_socket client(ioc);
        client.open();

        capy::run_async(ex)(
            [](local_stream_acceptor& a, local_stream_socket& s,
               std::error_code& ec_out, bool& done) -> capy::task<> {
                auto [ec] = co_await a.accept(s);
                ec_out    = ec;
                done      = true;
            }(acc, server, accept_ec, accept_done));

        capy::run_async(ex)(
            [](local_stream_socket& s, local_endpoint ep,
               std::error_code& ec_out, bool& done) -> capy::task<> {
                auto [ec] = co_await s.connect(ep);
                ec_out    = ec;
                done      = true;
            }(client, local_endpoint(path), connect_ec, connect_done));

        ioc.run();
        ioc.restart();

        BOOST_TEST_EQ(accept_done, true);
        BOOST_TEST_EQ(!accept_ec, true);
        BOOST_TEST_EQ(connect_done, true);
        BOOST_TEST_EQ(!connect_ec, true);
        BOOST_TEST_EQ(server.is_open(), true);
        BOOST_TEST_EQ(client.is_open(), true);
    }

    void testMoveAccept()
    {
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        local_stream_acceptor acc(ioc);
        acc.open();
        auto ec = acc.bind(local_endpoint(path));
        BOOST_TEST_EQ(!ec, true);
        ec = acc.listen();
        BOOST_TEST_EQ(!ec, true);

        std::error_code accept_ec, connect_ec;
        bool accept_done = false, connect_done = false;
        bool server_open = false;

        local_stream_socket client(ioc);
        client.open();

        capy::run_async(ex)(
            [](local_stream_acceptor& a,
               std::error_code& ec_out, bool& open_out,
               bool& done) -> capy::task<> {
                auto [ec, peer] = co_await a.accept();
                ec_out   = ec;
                open_out = peer.is_open();
                done     = true;
            }(acc, accept_ec, server_open, accept_done));

        capy::run_async(ex)(
            [](local_stream_socket& s, local_endpoint ep,
               std::error_code& ec_out, bool& done) -> capy::task<> {
                auto [ec] = co_await s.connect(ep);
                ec_out    = ec;
                done      = true;
            }(client, local_endpoint(path), connect_ec, connect_done));

        ioc.run();
        ioc.restart();

        BOOST_TEST_EQ(accept_done, true);
        BOOST_TEST_EQ(!accept_ec, true);
        BOOST_TEST_EQ(server_open, true);
        BOOST_TEST_EQ(connect_done, true);
        BOOST_TEST_EQ(!connect_ec, true);
    }

    void testReadWrite()
    {
        io_context ioc(Backend);
        local_stream_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        auto ex = ioc.get_executor();

        char const msg[] = "hello unix sockets";
        char buf[64]     = {};
        std::error_code write_ec, read_ec;
        std::size_t written = 0, read_n = 0;
        bool write_done = false, read_done = false;

        capy::run_async(ex)(
            [](local_stream_socket& s, char const* data, std::size_t len,
               std::error_code& ec_out, std::size_t& n_out,
               bool& done) -> capy::task<> {
                auto [ec, n] = co_await capy::write(
                    s, capy::const_buffer(data, len));
                ec_out = ec;
                n_out  = n;
                done   = true;
            }(s1, msg, std::strlen(msg), write_ec, written, write_done));

        capy::run_async(ex)(
            [](local_stream_socket& s, char* data, std::size_t len,
               std::error_code& ec_out, std::size_t& n_out,
               bool& done) -> capy::task<> {
                auto [ec, n] = co_await s.read_some(
                    capy::mutable_buffer(data, len));
                ec_out = ec;
                n_out  = n;
                done   = true;
            }(s2, buf, sizeof(buf), read_ec, read_n, read_done));

        ioc.run();
        ioc.restart();

        BOOST_TEST_EQ(write_done, true);
        BOOST_TEST_EQ(!write_ec, true);
        BOOST_TEST_EQ(written, std::strlen(msg));
        BOOST_TEST_EQ(read_done, true);
        BOOST_TEST_EQ(!read_ec, true);
        BOOST_TEST_EQ(read_n, std::strlen(msg));
        BOOST_TEST_EQ(std::string(buf, read_n), std::string(msg));
    }

    void testSocketPair()
    {
        io_context ioc(Backend);
        local_stream_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        BOOST_TEST_EQ(s1.is_open(), true);
        BOOST_TEST_EQ(s2.is_open(), true);
    }

    void testUnlinkExisting()
    {
        io_context ioc(Backend);
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        // First bind creates the socket file
        {
            local_stream_acceptor acc(ioc);
            acc.open();
            auto ec = acc.bind(local_endpoint(path));
            BOOST_TEST_EQ(!ec, true);
        }

        // Second bind without unlink_existing should fail
        {
            local_stream_acceptor acc(ioc);
            acc.open();
            auto ec = acc.bind(local_endpoint(path));
            BOOST_TEST_EQ(!!ec, true);
        }

        // Third bind with unlink_existing should succeed
        {
            local_stream_acceptor acc(ioc);
            acc.open();
            auto ec = acc.bind(
                local_endpoint(path), bind_option::unlink_existing);
            BOOST_TEST_EQ(!ec, true);
        }
    }

    void testUnlinkNonexistent()
    {
        // unlink_existing on a path that doesn't exist should
        // succeed (unlink silently fails with ENOENT).
        io_context ioc(Backend);
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        local_stream_acceptor acc(ioc);
        acc.open();
        auto ec = acc.bind(
            local_endpoint(path), bind_option::unlink_existing);
        BOOST_TEST_EQ(!ec, true);
    }

    void testEndpointOrdering()
    {
        local_endpoint a("/tmp/a");
        local_endpoint b("/tmp/b");
        local_endpoint a2("/tmp/a");
        local_endpoint prefix("/tmp");
        local_endpoint empty;

        // Equality
        BOOST_TEST_EQ(a == a2, true);
        BOOST_TEST_EQ(a != b, true);

        // Ordering
        BOOST_TEST_EQ(a < b, true);
        BOOST_TEST_EQ(b > a, true);
        BOOST_TEST_EQ(a <= a2, true);
        BOOST_TEST_EQ(a >= a2, true);

        // Prefix is less than full path
        BOOST_TEST_EQ(prefix < a, true);

        // Empty is less than everything
        BOOST_TEST_EQ(empty < a, true);
        BOOST_TEST_EQ(empty < prefix, true);

        // Spaceship
        BOOST_TEST_EQ((a <=> a2) == std::strong_ordering::equal, true);
        BOOST_TEST_EQ((a <=> b) == std::strong_ordering::less, true);
    }

    void testMoveAssign()
    {
        io_context ioc(Backend);
        local_stream_socket s1(ioc);
        local_stream_socket s2(ioc);
        s1.open();
        BOOST_TEST_EQ(s1.is_open(), true);
        BOOST_TEST_EQ(s2.is_open(), false);

        s2 = std::move(s1);
        BOOST_TEST_EQ(s2.is_open(), true);
        BOOST_TEST_EQ(s1.is_open(), false);

        // Self-move-assign is a no-op
        local_stream_socket& alias = s2;
        s2 = std::move(alias);
        BOOST_TEST_EQ(s2.is_open(), true);
    }

    void testCancelOnClosedSocket()
    {
        io_context ioc(Backend);
        local_stream_socket sock(ioc);

        // cancel() on a closed socket is a no-op (early return).
        sock.cancel();
        BOOST_TEST_EQ(sock.is_open(), false);
    }

    void testNativeHandleClosed()
    {
        io_context ioc(Backend);
        local_stream_socket sock(ioc);

#if BOOST_COROSIO_HAS_IOCP
        auto const invalid = static_cast<native_handle_type>(~0ull);
#else
        auto const invalid = static_cast<native_handle_type>(-1);
#endif
        BOOST_TEST(sock.native_handle() == invalid);

        sock.open();
        BOOST_TEST(sock.native_handle() != invalid);
        sock.close();
    }

    void testEndpointsClosed()
    {
        io_context ioc(Backend);
        local_stream_socket sock(ioc);

        // Endpoints on a closed socket are empty defaults
        BOOST_TEST_EQ(sock.local_endpoint().empty(), true);
        BOOST_TEST_EQ(sock.remote_endpoint().empty(), true);
    }

    void testEndpointsConnected()
    {
        io_context ioc(Backend);
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        local_stream_acceptor acc(ioc);
        acc.open();
        auto ec = acc.bind(local_endpoint(path));
        BOOST_TEST_EQ(!ec, true);
        ec = acc.listen();
        BOOST_TEST_EQ(!ec, true);

        local_stream_socket server(ioc);
        local_stream_socket client(ioc);
        auto ex = ioc.get_executor();

        capy::run_async(ex)(
            [](local_stream_acceptor& a, local_stream_socket& s)
                -> capy::task<> {
                (void)co_await a.accept(s);
            }(acc, server));

        capy::run_async(ex)(
            [](local_stream_socket& s, local_endpoint ep) -> capy::task<> {
                (void)co_await s.connect(ep);
            }(client, local_endpoint(path)));

        ioc.run();
        ioc.restart();

        // Endpoint accessors hit the backend
        auto cl = client.local_endpoint();
        auto cr = client.remote_endpoint();
        auto sl = server.local_endpoint();
        auto sr = server.remote_endpoint();
        // server local should match the listening path
        BOOST_TEST_EQ(sl.path(), path);
        // client remote should match the listening path
        BOOST_TEST_EQ(cr.path(), path);
        // touch the others so the lines exec
        (void)cl;
        (void)sr;
    }

    void testShutdown()
    {
        io_context ioc(Backend);
        local_stream_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        // Throwing overload (best-effort)
        s1.shutdown(shutdown_send);

        // Non-throwing overload
        std::error_code ec;
        s2.shutdown(shutdown_send, ec);
        // ec may be unset or ENOTCONN depending on backend; we just want
        // the code path exercised. The doc says best-effort.

        // Closed-socket variants
        local_stream_socket closed(ioc);
        closed.shutdown(shutdown_send);

        std::error_code ec2;
        closed.shutdown(shutdown_send, ec2);
        BOOST_TEST_EQ(!ec2, true);
    }

    void testAssignAlreadyOpenThrows()
    {
        io_context ioc(Backend);
        local_stream_socket sock(ioc);
        sock.open();
        BOOST_TEST_EQ(sock.is_open(), true);

        bool caught = false;
        try
        {
            sock.assign(static_cast<native_handle_type>(-1));
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testReleaseClosedThrows()
    {
        io_context ioc(Backend);
        local_stream_socket sock(ioc);

        bool caught = false;
        try
        {
            (void)sock.release();
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testAvailableClosedThrows()
    {
        io_context ioc(Backend);
        local_stream_socket sock(ioc);

        bool caught = false;
        try
        {
            (void)sock.available();
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testConnectToNonexistent()
    {
        // connect() to a path that doesn't exist should fail
        // gracefully with an error in the awaitable, no throw.
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        // temp dir exists, but the socket file inside it does not
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        local_stream_socket client(ioc);
        std::error_code result_ec;
        bool done = false;

        capy::run_async(ex)(
            [](local_stream_socket& s, local_endpoint ep,
               std::error_code& ec_out, bool& d) -> capy::task<> {
                auto [ec] = co_await s.connect(ep);
                ec_out = ec;
                d      = true;
            }(client, local_endpoint(path), result_ec, done));

        // Watchdog: if the platform parks the doomed connect instead
        // of failing it, retract it so the test reports the miss
        // instead of hanging the suite.
        auto watchdog = [&]() -> capy::task<> {
            (void)co_await corosio::delay(std::chrono::milliseconds(250));
            if (!done)
                client.cancel();
        };
        capy::run_async(ex)(watchdog());

        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(!!result_ec);
    }

    void testCancelPendingAccept()
    {
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        local_stream_acceptor acc(ioc);
        acc.open();
        auto ec = acc.bind(local_endpoint(path));
        BOOST_TEST_EQ(!ec, true);
        ec = acc.listen();
        BOOST_TEST_EQ(!ec, true);

        std::error_code accept_ec;
        bool accept_done = false;
        local_stream_socket server(ioc);

        capy::run_async(ex)(
            [](local_stream_acceptor& a, local_stream_socket& s,
               std::error_code& ec_out, bool& done) -> capy::task<> {
                auto [ec] = co_await a.accept(s);
                ec_out    = ec;
                done      = true;
            }(acc, server, accept_ec, accept_done));

        // Schedule a cancel after a brief delay
        auto canceller = [&]() -> capy::task<> {
            (void)co_await corosio::delay(std::chrono::milliseconds(20));
            acc.cancel();
        };
        capy::run_async(ex)(canceller());

        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(accept_ec == capy::cond::canceled);
    }

    // Stop-token cancel of a parked accept. Unlike
    // testCancelPendingAccept (acceptor-wide cancel()), this routes
    // through the per-waiter stop callback.
    void testStopTokenAccept()
    {
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        test::temp_socket_dir tmp;

        local_stream_acceptor acc(ioc);
        acc.open();
        auto ec = acc.bind(local_endpoint(tmp.path()));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);

        std::stop_source ss;
        local_stream_socket server(ioc);
        std::error_code accept_ec;
        bool accept_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [aec]  = co_await acc.accept(server);
            accept_ec   = aec;
            accept_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            (void)co_await corosio::delay(std::chrono::milliseconds(20));
            ss.request_stop();
        };

        capy::run_async(ex, ss.get_token())(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(accept_ec == capy::cond::canceled);
    }

    // wait(wait_type::error) has no immediate-completion path; it
    // parks until cancel() retracts it. On IOCP this routes through
    // the auxiliary wait reactor rather than the completion port.
    void testWaitErrorCancel()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        local_stream_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::error);
            wait_ec   = ec;
            wait_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            (void)co_await corosio::delay(std::chrono::milliseconds(20));
            s1.cancel();
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    // Socket-wide cancel() of a parked read retracts the in-flight
    // operation and completes it with capy::cond::canceled.
    void testCancelPendingRead()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        local_stream_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        std::error_code read_ec;
        bool read_done = false;
        char buf[16];

        auto reader = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            (void)n;
            read_ec   = ec;
            read_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            (void)co_await corosio::delay(std::chrono::milliseconds(20));
            s1.cancel();
        };

        capy::run_async(ex)(reader());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(read_done);
        BOOST_TEST(read_ec == capy::cond::canceled);
    }

    // Stop-token cancel of a parked read routes through the per-op
    // stop callback rather than the socket-wide cancel.
    void testStopTokenRead()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        local_stream_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        std::stop_source ss;
        std::error_code read_ec;
        bool read_done = false;
        char buf[16];

        auto reader = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            (void)n;
            read_ec   = ec;
            read_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            (void)co_await corosio::delay(std::chrono::milliseconds(20));
            ss.request_stop();
        };

        capy::run_async(ex, ss.get_token())(reader());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(read_done);
        BOOST_TEST(read_ec == capy::cond::canceled);
    }

    // Zero-length reads and writes complete immediately with zero
    // bytes and no error; a zero-byte stream read is not EOF.
    void testEmptyBufferOps()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        local_stream_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        std::error_code write_ec, read_ec;
        std::size_t write_n = 1, read_n = 1;

        auto io = [&]() -> capy::task<> {
            {
                auto [ec, n] =
                    co_await s1.write_some(capy::const_buffer(nullptr, 0));
                write_ec = ec;
                write_n  = n;
            }
            {
                auto [ec, n] =
                    co_await s1.read_some(capy::mutable_buffer(nullptr, 0));
                read_ec = ec;
                read_n  = n;
            }
        };

        capy::run_async(ex)(io());
        ioc.run();

        BOOST_TEST(!write_ec);
        BOOST_TEST_EQ(write_n, 0u);
        BOOST_TEST(!read_ec);
        BOOST_TEST_EQ(read_n, 0u);
    }

    void testOptions()
    {
        io_context ioc(Backend);
        local_stream_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        // AF_UNIX option support varies by platform; the point is to
        // drive the set/get paths, so accept a system error as a
        // valid outcome.
        try
        {
            s1.set_option(socket_option::send_buffer_size(16384));
            auto opt = s1.get_option<socket_option::send_buffer_size>();
            BOOST_TEST(opt.value() > 0);
        }
        catch (std::system_error const&)
        {
            BOOST_TEST_PASS();
        }

        local_stream_socket closed(ioc);
        BOOST_TEST_THROWS(
            closed.set_option(socket_option::send_buffer_size(4096)),
            std::logic_error);
        BOOST_TEST_THROWS(
            (void)closed.get_option<socket_option::send_buffer_size>(),
            std::logic_error);
    }

    void testAcceptorOptions()
    {
        io_context ioc(Backend);
        test::temp_socket_dir tmp;

        local_stream_acceptor acc(ioc);
        acc.open();

        // AF_UNIX option support varies by platform; the point is to
        // drive the set/get paths, so accept a system error as a
        // valid outcome.
        try
        {
            acc.set_option(socket_option::reuse_address(true));
            (void)acc.get_option<socket_option::reuse_address>();
        }
        catch (std::system_error const&)
        {
            BOOST_TEST_PASS();
        }
        acc.close();

        local_stream_acceptor closed(ioc);
        BOOST_TEST_THROWS(
            closed.set_option(socket_option::reuse_address(true)),
            std::logic_error);
        BOOST_TEST_THROWS(
            (void)closed.get_option<socket_option::reuse_address>(),
            std::logic_error);
    }

    // Acceptor wait(wait_type::write) completes immediately: a listener
    // is always writable by convention.
    void testAcceptorWaitWrite()
    {
#if BOOST_COROSIO_HAS_IO_URING
        // The immediate-writable convention is a reactor/IOCP behavior;
        // io_uring's poll never reports a listener writable, so the
        // wait would park forever.
        if constexpr (std::is_same_v<
                std::remove_const_t<decltype(Backend)>, io_uring_t>)
            return;
#endif
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        test::temp_socket_dir tmp;

        local_stream_acceptor acc(ioc);
        acc.open();
        auto ec = acc.bind(local_endpoint(tmp.path()));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [wec] = co_await acc.wait(wait_type::write);
            wait_ec    = wec;
            wait_done  = true;
        };

        capy::run_async(ex)(waiter());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    // Acceptor wait(wait_type::read) parks until a cancel retracts it.
    void testAcceptorWaitReadCancel()
    {
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        test::temp_socket_dir tmp;

        local_stream_acceptor acc(ioc);
        acc.open();
        auto ec = acc.bind(local_endpoint(tmp.path()));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [wec] = co_await acc.wait(wait_type::read);
            wait_ec    = wec;
            wait_done  = true;
        };
        auto canceller = [&]() -> capy::task<> {
            (void)co_await corosio::delay(std::chrono::milliseconds(20));
            acc.cancel();
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

#if BOOST_COROSIO_POSIX
    // Accept a connection that is already queued in the listen backlog
    // before the io_context ever runs. The accept can then complete on
    // the immediate path instead of parking a waiter. Uses raw POSIX
    // socket calls to connect without driving the io_context.
    void testAcceptPendingConnection()
    {
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        local_stream_acceptor acc(ioc);
        acc.open();
        auto ec = acc.bind(local_endpoint(path));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);

        // Raw blocking connect: completes via the kernel's listen
        // backlog without the io_context running.
        int cfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        BOOST_TEST(cfd >= 0);
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        std::strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
        int crc = ::connect(
            cfd, reinterpret_cast<sockaddr const*>(&sa), sizeof(sa));
        BOOST_TEST_EQ(crc, 0);

        local_stream_socket server(ioc);
        std::error_code accept_ec;
        bool accept_done = false;

        auto acceptor_task = [&]() -> capy::task<> {
            auto [aec]  = co_await acc.accept(server);
            accept_ec   = aec;
            accept_done = true;
        };
        capy::run_async(ex)(acceptor_task());
        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(server.is_open());
        ::close(cfd);
    }
#endif // BOOST_COROSIO_POSIX

    // accept() on an open, bound, but non-listening socket fails with
    // a system error instead of hanging.
    void testAcceptWithoutListen()
    {
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        test::temp_socket_dir tmp;

        local_stream_acceptor acc(ioc);
        acc.open();
        auto ec = acc.bind(local_endpoint(tmp.path()));
        BOOST_TEST(!ec);

        local_stream_socket server(ioc);
        std::error_code accept_ec;
        bool accept_done = false;

        auto acceptor_task = [&]() -> capy::task<> {
            auto [aec]  = co_await acc.accept(server);
            accept_ec   = aec;
            accept_done = true;
        };
        capy::run_async(ex)(acceptor_task());

        // Watchdog: if the platform parks the accept instead of
        // failing it, retract it so the test reports the miss
        // instead of hanging the suite.
        auto watchdog = [&]() -> capy::task<> {
            (void)co_await corosio::delay(std::chrono::milliseconds(250));
            if (!accept_done)
                acc.cancel();
        };
        capy::run_async(ex)(watchdog());

        ioc.run();

        BOOST_TEST(accept_done);
        // Exact errno is platform-dependent (EINVAL on Linux); only
        // require that an error is reported.
        BOOST_TEST(accept_ec);
        BOOST_TEST(!server.is_open());
    }

    // Destroy the io_context with an accept still parked; service
    // shutdown must release the waiter without resuming it.
    void testDestroyWithParkedAccept()
    {
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        test::temp_socket_dir tmp;

        local_stream_acceptor acc(ioc);
        acc.open();
        auto ec = acc.bind(local_endpoint(tmp.path()));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);

        local_stream_socket server(ioc);

        auto acceptor_task = [&]() -> capy::task<> {
            (void)co_await acc.accept(server);
        };
        capy::run_async(ex)(acceptor_task());

        // Run the coroutine to its parked suspension point only, then
        // fall off the end of the scope with the accept outstanding.
        (void)ioc.run_one();
        BOOST_TEST_PASS();
    }

    // Destroy the io_context with a read still parked on a connected
    // pair; the socket service's shutdown must drain the abandoned
    // operation without resuming it.
    void testDestroyWithParkedRead()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        local_stream_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        char buf[16];
        auto reader = [&]() -> capy::task<> {
            (void)co_await s1.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
        };
        capy::run_async(ex)(reader());

        (void)ioc.run_one();
        BOOST_TEST_PASS();
    }

    void testAcceptorOnClosedNoOp()
    {
        // cancel/close on a never-opened acceptor are no-ops.
        io_context ioc(Backend);
        local_stream_acceptor acc(ioc);
        BOOST_TEST_EQ(acc.is_open(), false);

        acc.cancel();
        acc.close();
        BOOST_TEST_EQ(acc.is_open(), false);

        // local_endpoint() on a closed acceptor returns an empty endpoint.
        BOOST_TEST_EQ(acc.local_endpoint().empty(), true);
    }

    void testAcceptorBindClosedThrows()
    {
        io_context ioc(Backend);
        local_stream_acceptor acc(ioc);
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        BOOST_TEST_THROWS(acc.bind(local_endpoint("/tmp/never")),
                          std::logic_error);
    }

    void testAcceptorListenClosedThrows()
    {
        io_context ioc(Backend);
        local_stream_acceptor acc(ioc);
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        BOOST_TEST_THROWS(acc.listen(), std::logic_error);
    }

    void testAcceptorAcceptClosedThrows()
    {
        io_context ioc(Backend);
        local_stream_acceptor acc(ioc);
        local_stream_socket peer(ioc);

        bool caught_peer = false;
        try
        {
            (void)acc.accept(peer);
        }
        catch (std::logic_error const&)
        {
            caught_peer = true;
        }
        BOOST_TEST(caught_peer);

        bool caught_move = false;
        try
        {
            (void)acc.accept();
        }
        catch (std::logic_error const&)
        {
            caught_move = true;
        }
        BOOST_TEST(caught_move);
    }

    void testAcceptorReleaseClosedThrows()
    {
        io_context ioc(Backend);
        local_stream_acceptor acc(ioc);

        bool caught = false;
        try
        {
            (void)acc.release();
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testAcceptorReleaseOpen()
    {
        // release() returns the native fd and closes the acceptor
        io_context ioc(Backend);
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        local_stream_acceptor acc(ioc);
        acc.open();
        auto ec = acc.bind(local_endpoint(path));
        BOOST_TEST_EQ(!ec, true);
        ec = acc.listen();
        BOOST_TEST_EQ(!ec, true);

        BOOST_TEST_EQ(acc.is_open(), true);
        auto h = acc.release();
        (void)h;
        BOOST_TEST_EQ(acc.is_open(), false);

#if BOOST_COROSIO_POSIX
        if (static_cast<int>(h) >= 0)
            ::close(static_cast<int>(h));
#endif
    }

    void testAcceptorLocalEndpoint()
    {
        io_context ioc(Backend);
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        local_stream_acceptor acc(ioc);
        acc.open();
        auto ec = acc.bind(local_endpoint(path));
        BOOST_TEST_EQ(!ec, true);

        auto ep = acc.local_endpoint();
        BOOST_TEST_EQ(ep.path(), path);
    }

    void testEndpointTooLongThrows()
    {
        std::string too_long(local_endpoint::max_path_length + 1, 'x');
        bool caught = false;
        try
        {
            local_endpoint ep(too_long);
            (void)ep;
        }
        catch (std::system_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testEndpointTooLongNoThrow()
    {
        std::string too_long(local_endpoint::max_path_length + 1, 'x');
        std::error_code ec;
        local_endpoint ep(too_long, ec);
        BOOST_TEST(!!ec);
        BOOST_TEST_EQ(ep.empty(), true);

        // Successful construction with the no-throw overload clears ec.
        std::error_code ec2;
        local_endpoint ep2("/tmp/ok", ec2);
        BOOST_TEST_EQ(!ec2, true);
        BOOST_TEST_EQ(ep2.path(), std::string_view("/tmp/ok"));
    }

    void testEndpointMaxPathLength()
    {
        // Exactly at the limit should succeed.
        std::string at_limit(local_endpoint::max_path_length, 'a');
        local_endpoint ep(at_limit);
        BOOST_TEST_EQ(ep.path().size(), local_endpoint::max_path_length);
    }

#ifdef __linux__
    void testAbstractEndpoint()
    {
        std::string abs_path(1, '\0');
        abs_path += "corosio_test_abstract_endpoint";
        local_endpoint ep(abs_path);
        BOOST_TEST(ep.is_abstract());
        BOOST_TEST_EQ(ep.empty(), false);
    }
#endif

    void run()
    {
        testConstruction();
        testOpen();
        testMove();
        testMoveAssign();
        testCancelOnClosedSocket();
        testNativeHandleClosed();
        testEndpointsClosed();
        testConnectAccept();
        testMoveAccept();
        testReadWrite();
        testSocketPair();
        testEndpointsConnected();
        testShutdown();
        testAssignAlreadyOpenThrows();
        testReleaseClosedThrows();
        testAvailableClosedThrows();
        testConnectToNonexistent();
        testCancelPendingAccept();
        testStopTokenAccept();
        testWaitErrorCancel();
        testCancelPendingRead();
        testStopTokenRead();
        testEmptyBufferOps();
        testOptions();
        testAcceptorOptions();
        testAcceptorWaitWrite();
        testAcceptorWaitReadCancel();
#if BOOST_COROSIO_POSIX
        testAcceptPendingConnection();
#endif
        testAcceptWithoutListen();
#if !COROSIO_TEST_HAS_ASAN
        // Abandon parked coroutine frames by design; see context.hpp.
        testDestroyWithParkedAccept();
        testDestroyWithParkedRead();
#endif
        testAcceptorOnClosedNoOp();
        testAcceptorBindClosedThrows();
        testAcceptorListenClosedThrows();
        testAcceptorAcceptClosedThrows();
        testAcceptorReleaseClosedThrows();
        testAcceptorReleaseOpen();
        testAcceptorLocalEndpoint();
        testEndpointTooLongThrows();
        testEndpointTooLongNoThrow();
        testEndpointMaxPathLength();
#ifdef __linux__
        testAbstractEndpoint();
#endif
        testUnlinkExisting();
        testUnlinkNonexistent();
        testEndpointOrdering();
        testEndpointStreamOutput();
        testAvailable();
        testRelease();
    }

    void testAvailable()
    {
        io_context ioc(Backend);
        local_stream_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        // Nothing written yet
        BOOST_TEST_EQ(s2.available(), std::size_t(0));

        // Write some data synchronously through the pair
        char const msg[] = "available test";
        auto ex = ioc.get_executor();
        bool done = false;

        capy::run_async(ex)(
            [](local_stream_socket& s, char const* data, std::size_t len,
               bool& d) -> capy::task<> {
                (void)co_await capy::write(s, capy::const_buffer(data, len));
                d = true;
            }(s1, msg, std::strlen(msg), done));

        ioc.run();
        ioc.restart();

        BOOST_TEST_EQ(done, true);
        BOOST_TEST_EQ(s2.available(), std::strlen(msg));
    }

    // Writes through the released handle with raw platform calls to
    // prove ownership actually transferred.
    void testRelease()
    {
        io_context ioc(Backend);
        local_stream_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        BOOST_TEST_EQ(s1.is_open(), true);

        auto handle = s1.release();
        BOOST_TEST_EQ(s1.is_open(), false);

        // The released handle is still valid -- write through it
        char const msg[] = "released";
#if BOOST_COROSIO_HAS_IOCP
        BOOST_TEST_EQ(
            ::send(static_cast<SOCKET>(handle),
                   msg, static_cast<int>(std::strlen(msg)), 0) > 0, true);
        ::closesocket(static_cast<SOCKET>(handle));
#else
        BOOST_TEST_EQ(handle >= 0, true);
        BOOST_TEST_EQ(::write(handle, msg, std::strlen(msg)) > 0, true);
        ::close(handle);
#endif
    }

    void testEndpointStreamOutput()
    {
        // Non-abstract path
        {
            std::ostringstream os;
            os << local_endpoint("/tmp/sock");
            BOOST_TEST_EQ(os.str(), std::string("/tmp/sock"));
        }

        // Empty endpoint
        {
            std::ostringstream os;
            os << local_endpoint();
            BOOST_TEST_EQ(os.str(), std::string(""));
        }

#ifdef __linux__
        // Abstract socket
        {
            std::string abs_path(1, '\0');
            abs_path += "test_name";
            std::ostringstream os;
            os << local_endpoint(abs_path);
            BOOST_TEST_EQ(os.str(), std::string("[abstract:test_name]"));
        }
#endif
    }
};

COROSIO_BACKEND_TESTS(
    local_stream_socket_test, "boost.corosio.local_stream_socket")

} // namespace boost::corosio
