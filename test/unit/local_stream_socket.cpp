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
#include <boost/corosio/local_datagram_socket.hpp>
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
#include <fcntl.h>
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
#include <tuple>
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

        BOOST_TEST(!sock.open());
        BOOST_TEST_EQ(sock.is_open(), true);

        sock.close();
        BOOST_TEST_EQ(sock.is_open(), false);
    }

    void testMove()
    {
        io_context ioc(Backend);
        local_stream_socket s1(ioc);
        BOOST_TEST(!s1.open());
        BOOST_TEST_EQ(s1.is_open(), true);

        local_stream_socket s2(std::move(s1));
        BOOST_TEST_EQ(s2.is_open(), true);
        BOOST_TEST_EQ(s1.is_open(), false);
    }

    void testAcceptorConvenienceConstructor()
    {
        io_context ioc(Backend);
        test::temp_socket_dir tmp;
        local_endpoint ep(tmp.path());

        local_stream_acceptor acc(ioc, ep);
        BOOST_TEST(acc.is_open());

        // Opening an already-open acceptor is a no-op reporting success.
        BOOST_TEST(!acc.open());
        BOOST_TEST(acc.is_open());
        BOOST_TEST_EQ(acc.local_endpoint().path(), tmp.path());

        // A second acceptor on the same path surfaces the bind
        // conflict by throwing system_error.
        std::error_code caught;
        try
        {
            local_stream_acceptor dup(ioc, ep);
            BOOST_TEST_FAIL();
        }
        catch (std::system_error const& e)
        {
            caught = e.code();
        }
        BOOST_TEST(caught == std::errc::address_in_use);

        acc.close();
    }

    void testConnectAccept()
    {
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        local_stream_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        auto ec = acc.bind(local_endpoint(path));
        BOOST_TEST_EQ(!ec, true);
        ec = acc.listen();
        BOOST_TEST_EQ(!ec, true);

        std::error_code accept_ec, connect_ec;
        bool accept_done = false, connect_done = false;

        local_stream_socket server(ioc);
        local_stream_socket client(ioc);
        BOOST_TEST(!client.open());

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
        BOOST_TEST(!acc.open());
        auto ec = acc.bind(local_endpoint(path));
        BOOST_TEST_EQ(!ec, true);
        ec = acc.listen();
        BOOST_TEST_EQ(!ec, true);

        std::error_code accept_ec, connect_ec;
        bool accept_done = false, connect_done = false;
        bool server_open = false;

        local_stream_socket client(ioc);
        BOOST_TEST(!client.open());

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
            BOOST_TEST(!acc.open());
            auto ec = acc.bind(local_endpoint(path));
            BOOST_TEST_EQ(!ec, true);
        }

        // Second bind without unlink_existing should fail
        {
            local_stream_acceptor acc(ioc);
            BOOST_TEST(!acc.open());
            auto ec = acc.bind(local_endpoint(path));
            BOOST_TEST_EQ(!!ec, true);
        }

        // Third bind with unlink_existing should succeed
        {
            local_stream_acceptor acc(ioc);
            BOOST_TEST(!acc.open());
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
        BOOST_TEST(!acc.open());
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
        BOOST_TEST(!s1.open());
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

        BOOST_TEST(!sock.open());
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
        BOOST_TEST(!acc.open());
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
                std::ignore = co_await a.accept(s);
            }(acc, server));

        capy::run_async(ex)(
            [](local_stream_socket& s, local_endpoint ep) -> capy::task<> {
                std::ignore = co_await s.connect(ep);
            }(client, local_endpoint(path)));

        ioc.run();
        ioc.restart();

        // Endpoint accessors hit the backend
        [[maybe_unused]] auto cl = client.local_endpoint();
        auto cr = client.remote_endpoint();
        auto sl = server.local_endpoint();
        [[maybe_unused]] auto sr = server.remote_endpoint();
        // server local should match the listening path
        BOOST_TEST_EQ(sl.path(), path);
        // client remote should match the listening path
        BOOST_TEST_EQ(cr.path(), path);
        // touch the others so the lines exec
    }

    void testShutdown()
    {
        io_context ioc(Backend);
        local_stream_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        BOOST_TEST(!s1.shutdown(shutdown_send));
        BOOST_TEST(!s2.shutdown(shutdown_send));

        // Closed socket reports bad_file_descriptor
        local_stream_socket closed(ioc);
        BOOST_TEST(closed.shutdown(shutdown_send)
                   == std::errc::bad_file_descriptor);
    }

#if BOOST_COROSIO_POSIX
    // Assign over an open socket cancels its pending operations and
    // adopts, matching the internet family.
    void testAssignOverOpenAdopts()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        local_stream_socket s1(ioc), s2(ioc);
        BOOST_TEST(!connect_pair(s1, s2));

        int fds[2];
        BOOST_TEST(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        int fl = ::fcntl(fds[0], F_GETFL);
        BOOST_TEST(::fcntl(fds[0], F_SETFL, fl | O_NONBLOCK) == 0);

        bool read_done = false;
        std::error_code read_ec;
        auto reader = [&]() -> capy::task<> {
            char buf[4];
            [[maybe_unused]] auto [ec, n] = co_await s1.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            read_ec   = ec;
            read_done = true;
        };
        auto assigner = [&]() -> capy::task<> {
            BOOST_TEST(!s1.assign(static_cast<native_handle_type>(fds[0])));
            co_return;
        };
        capy::run_async(ex)(reader());
        capy::run_async(ex)(assigner());
        ioc.run();
        ioc.restart();

        BOOST_TEST(read_done);
        BOOST_TEST(read_ec == capy::cond::canceled);
        BOOST_TEST(s1.is_open());
        BOOST_TEST(
            s1.native_handle() == static_cast<native_handle_type>(fds[0]));

        // The adopted descriptor reaches its new peer.
        BOOST_TEST(::send(fds[1], "go", 2, 0) == 2);
        bool got = false;
        auto reread = [&]() -> capy::task<> {
            char buf[4];
            auto [ec, n] = co_await s1.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            got = !ec && n == 2;
        };
        capy::run_async(ex)(reread());
        ioc.run();
        BOOST_TEST(got);

        ::close(fds[1]);
    }

    // A failed assign over an open socket leaves it untouched and
    // functional.
    void testAssignOverOpenRejectedKeepsSocket()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        local_stream_socket s1(ioc), s2(ioc);
        BOOST_TEST(!connect_pair(s1, s2));

        int fds[2];
        BOOST_TEST(::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) == 0);

        BOOST_TEST(s1.assign(static_cast<native_handle_type>(fds[0]))
                   == std::errc::wrong_protocol_type);
        BOOST_TEST(::fcntl(fds[0], F_GETFD) >= 0); // caller keeps it
        BOOST_TEST(s1.is_open());

        // The rejected assign disturbed nothing: the pair still moves
        // bytes.
        bool got = false;
        auto writer = [&]() -> capy::task<> {
            char const out[] = "ok";
            [[maybe_unused]] auto [wec, wn] = co_await s2.write_some(
                capy::const_buffer(out, 2));
            BOOST_TEST(!wec);
        };
        auto reader = [&]() -> capy::task<> {
            char buf[4];
            auto [ec, n] = co_await s1.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            got = !ec && n == 2;
        };
        capy::run_async(ex)(reader());
        capy::run_async(ex)(writer());
        ioc.run();
        BOOST_TEST(got);

        ::close(fds[0]);
        ::close(fds[1]);
    }
#endif

#if BOOST_COROSIO_POSIX
    // Backend validation, not just the front-end guard: a bad fd must
    // be rejected on every backend.
    void testAssignBadFdThrows()
    {
        io_context ioc(Backend);
        local_stream_socket sock(ioc);
        BOOST_TEST(sock.assign((native_handle_type)-1)
                   == std::errc::bad_file_descriptor);
        BOOST_TEST(!sock.is_open());
    }

    // A rejected fd must remain owned and usable by the caller
    // (validation happens before any state is touched).
    void testAssignRejectedFdStaysOpen()
    {
        io_context ioc(Backend);
        local_stream_socket sock(ioc);
        int fds[2];
        BOOST_TEST(::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) == 0);
        BOOST_TEST(sock.assign((native_handle_type)fds[0])
                   == std::errc::wrong_protocol_type);
        // fd still valid: fcntl succeeds
        BOOST_TEST(::fcntl(fds[0], F_GETFD) >= 0);
        BOOST_TEST(!sock.is_open());
        ::close(fds[0]);
        ::close(fds[1]);
    }
#endif

#if BOOST_COROSIO_HAS_EPOLL
    // Adopting an fd the reactor already tracks must fail with an
    // error, not terminate. epoll reports EEXIST; kqueue and select
    // accept re-registration, so only epoll is asserted.
    void testAssignDuplicateFdErrors()
    {
        if constexpr (std::is_same_v<
                std::remove_const_t<decltype(Backend)>, epoll_t>)
        {
            io_context ioc(Backend);
            local_stream_socket a(ioc);
            local_stream_socket b(ioc);
            int fds[2];
            BOOST_TEST(::socketpair(
                AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0);
            BOOST_TEST(!a.assign((native_handle_type)fds[0]));
            BOOST_TEST(b.assign((native_handle_type)fds[0]));
            BOOST_TEST(a.is_open());
            ::close(fds[1]);
        }
    }
#endif

    void testReleaseClosedThrows()
    {
        io_context ioc(Backend);
        local_stream_socket sock(ioc);

        std::error_code caught;
        try
        {
            std::ignore = sock.release();
        }
        catch (std::system_error const& e)
        {
            caught = e.code();
        }
        BOOST_TEST(caught == std::errc::bad_file_descriptor);
    }

    void testAvailableClosedThrows()
    {
        io_context ioc(Backend);
        local_stream_socket sock(ioc);

        std::error_code caught;
        try
        {
            std::ignore = sock.available();
        }
        catch (std::system_error const& e)
        {
            caught = e.code();
        }
        BOOST_TEST(caught == std::errc::bad_file_descriptor);
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
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(250));
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
        BOOST_TEST(!acc.open());
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
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(20));
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
        BOOST_TEST(!acc.open());
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
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(20));
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
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(20));
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
            [[maybe_unused]] auto [ec, n] = co_await s1.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            read_ec   = ec;
            read_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(20));
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
            [[maybe_unused]] auto [ec, n] = co_await s1.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            read_ec   = ec;
            read_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(20));
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
        auto expect_bad_fd = [](auto fn) {
            std::error_code caught;
            try { fn(); }
            catch (std::system_error const& e) { caught = e.code(); }
            BOOST_TEST(caught == std::errc::bad_file_descriptor);
        };
        expect_bad_fd([&] {
            closed.set_option(socket_option::send_buffer_size(4096)); });
        expect_bad_fd([&] {
            std::ignore =
                closed.get_option<socket_option::send_buffer_size>(); });
    }

    void testAcceptorOptions()
    {
        io_context ioc(Backend);
        test::temp_socket_dir tmp;

        local_stream_acceptor acc(ioc);
        BOOST_TEST(!acc.open());

        // AF_UNIX option support varies by platform; the point is to
        // drive the set/get paths, so accept a system error as a
        // valid outcome.
        try
        {
            acc.set_option(socket_option::reuse_address(true));
            std::ignore = acc.get_option<socket_option::reuse_address>();
        }
        catch (std::system_error const&)
        {
            BOOST_TEST_PASS();
        }
        acc.close();

        local_stream_acceptor closed(ioc);
        auto expect_bad_fd = [](auto fn) {
            std::error_code caught;
            try { fn(); }
            catch (std::system_error const& e) { caught = e.code(); }
            BOOST_TEST(caught == std::errc::bad_file_descriptor);
        };
        expect_bad_fd([&] {
            closed.set_option(socket_option::reuse_address(true)); });
        expect_bad_fd([&] {
            std::ignore =
                closed.get_option<socket_option::reuse_address>(); });
    }

    // Acceptor wait(wait_type::write) fails uniformly on every
    // backend: writability carries no meaning for a listener.
    void testAcceptorWaitWrite()
    {
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        test::temp_socket_dir tmp;

        local_stream_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
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
        BOOST_TEST(wait_ec == std::errc::operation_not_supported);
    }

    // Acceptor wait(wait_type::read) parks until a cancel retracts it.
    void testAcceptorWaitReadCancel()
    {
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        test::temp_socket_dir tmp;

        local_stream_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
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
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(20));
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
        BOOST_TEST(!acc.open());
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
        BOOST_TEST(!acc.open());
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
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(250));
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
        BOOST_TEST(!acc.open());
        auto ec = acc.bind(local_endpoint(tmp.path()));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);

        local_stream_socket server(ioc);

        auto acceptor_task = [&]() -> capy::task<> {
            std::ignore = co_await acc.accept(server);
        };
        capy::run_async(ex)(acceptor_task());

        // Run the coroutine to its parked suspension point only, then
        // fall off the end of the scope with the accept outstanding.
        std::ignore = ioc.run_one();
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
            std::ignore = co_await s1.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
        };
        capy::run_async(ex)(reader());

        std::ignore = ioc.run_one();
        BOOST_TEST_PASS();
    }

    // Destroy the io_context with a read completion already queued.
    // Both pairs are made readable before the loop runs, so the single
    // dispatched handler leaves the other completion behind and the
    // scheduler's shutdown has to drain it rather than deliver it.
    void testDestroyWithQueuedRead()
    {
        int resumed = 0;
        int before_destroy = 0;
        {
            io_context ioc(Backend);
            auto ex = ioc.get_executor();
            local_stream_socket a1(ioc), b1(ioc), a2(ioc), b2(ioc);
            if (auto ec = connect_pair(a1, b1))
                throw std::system_error(ec, "connect_pair");
            if (auto ec = connect_pair(a2, b2))
                throw std::system_error(ec, "connect_pair");

            char buf1[8], buf2[8];
            auto reader = [&](local_stream_socket& s,
                              char* p) -> capy::task<> {
                std::ignore = co_await s.read_some(
                    capy::mutable_buffer(p, 8));
                ++resumed;
            };
            capy::run_async(ex)(reader(a1, buf1));
            capy::run_async(ex)(reader(a2, buf2));
            // Two handlers, one per coroutine, park both reads.
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();

            BOOST_TEST(::send(b1.native_handle(), "x", 1, 0) == 1);
            BOOST_TEST(::send(b2.native_handle(), "x", 1, 0) == 1);

            std::ignore = ioc.run_one();
            before_destroy = resumed;
        }
        BOOST_TEST(before_destroy < 2);
        BOOST_TEST_EQ(resumed, before_destroy);
    }

    // Destroy the io_context with a connect completion already queued.
    // Both handshakes finish before the loop dispatches, so one is
    // still waiting in the queue when the scheduler shuts down.
    void testDestroyWithQueuedConnect()
    {
        int resumed        = 0;
        int before_destroy = 0;
        {
            io_context ioc(Backend);
            auto ex = ioc.get_executor();
            test::temp_socket_dir tmp;

            local_stream_acceptor acc(ioc);
            BOOST_TEST(!acc.open());
            auto const ep = local_endpoint(tmp.path());
            BOOST_TEST(!acc.bind(ep));
            BOOST_TEST(!acc.listen());

            local_stream_socket c1(ioc), c2(ioc);
            auto client = [&](local_stream_socket& s) -> capy::task<> {
                std::ignore = co_await s.connect(ep);
                ++resumed;
            };
            capy::run_async(ex)(client(c1));
            capy::run_async(ex)(client(c2));
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
            before_destroy = resumed;
        }
        BOOST_TEST_EQ(resumed, before_destroy);
    }

    // Destroy the io_context with a wait completion already queued.
    // Both sockets become readable while the waits are parked, so the
    // one handler the loop dispatches leaves the other wait for the
    // scheduler's shutdown to drain.
    void testDestroyWithQueuedWait()
    {
        int resumed        = 0;
        int before_destroy = 0;
        {
            io_context ioc(Backend);
            auto ex = ioc.get_executor();
            local_stream_socket a1(ioc), b1(ioc), a2(ioc), b2(ioc);
            if (auto ec = connect_pair(a1, b1))
                throw std::system_error(ec, "connect_pair");
            if (auto ec = connect_pair(a2, b2))
                throw std::system_error(ec, "connect_pair");

            auto waiter = [&](local_stream_socket& s) -> capy::task<> {
                std::ignore = co_await s.wait(wait_type::read);
                ++resumed;
            };
            capy::run_async(ex)(waiter(a1));
            capy::run_async(ex)(waiter(a2));
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();

            BOOST_TEST(::send(b1.native_handle(), "x", 1, 0) == 1);
            BOOST_TEST(::send(b2.native_handle(), "x", 1, 0) == 1);

            std::ignore = ioc.run_one();
            before_destroy = resumed;
        }
        BOOST_TEST(before_destroy < 2);
        BOOST_TEST_EQ(resumed, before_destroy);
    }

    // Destroy the io_context with a write completion already queued.
    // Only the proactor backends reach that state: a POSIX write to a
    // socket with room finishes in the initiator, so there the scope
    // ends with nothing outstanding and the test is a no-op.
    void testDestroyWithQueuedWrite()
    {
        int resumed        = 0;
        int before_destroy = 0;
        {
            io_context ioc(Backend);
            auto ex = ioc.get_executor();
            local_stream_socket a1(ioc), b1(ioc), a2(ioc), b2(ioc);
            if (auto ec = connect_pair(a1, b1))
                throw std::system_error(ec, "connect_pair");
            if (auto ec = connect_pair(a2, b2))
                throw std::system_error(ec, "connect_pair");

            auto writer = [&](local_stream_socket& s) -> capy::task<> {
                std::ignore =
                    co_await s.write_some(capy::const_buffer("x", 1));
                ++resumed;
            };
            capy::run_async(ex)(writer(a1));
            capy::run_async(ex)(writer(a2));
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
            before_destroy = resumed;
        }
        BOOST_TEST_EQ(resumed, before_destroy);
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
        BOOST_TEST(acc.bind(local_endpoint("/tmp/never"))
                   == std::errc::bad_file_descriptor);
    }

    void testAcceptorListenClosedThrows()
    {
        io_context ioc(Backend);
        local_stream_acceptor acc(ioc);
        BOOST_TEST(acc.listen() == std::errc::bad_file_descriptor);
    }

    void testOptionFailureThrows()
    {
        io_context ioc(Backend);

        // A TCP-level option on an AF_UNIX socket fails with a genuine
        // error surfaced as system_error
        local_stream_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");
        bool threw = false;
        try
        {
            s1.set_option(socket_option::no_delay(true));
        }
        catch (std::system_error const& e)
        {
            threw = bool(e.code());
        }
        BOOST_TEST(threw);

#if !defined(__APPLE__)
        // Darwin's getsockopt accepts TCP_NODELAY on AF_UNIX sockets,
        // so the get failure is only asserted where the kernel rejects it.
        bool sock_get_threw = false;
        try
        {
            std::ignore = s1.get_option<socket_option::no_delay>();
        }
        catch (std::system_error const& e)
        {
            sock_get_threw = bool(e.code());
        }
        BOOST_TEST(sock_get_threw);
#endif

        // Same through the acceptor's option surface
        local_stream_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        bool acc_threw = false;
        try
        {
            acc.set_option(socket_option::no_delay(true));
        }
        catch (std::system_error const& e)
        {
            acc_threw = bool(e.code());
        }
        BOOST_TEST(acc_threw);
        bool get_threw = false;
        try
        {
            std::ignore = acc.get_option<socket_option::no_delay>();
        }
        catch (std::system_error const& e)
        {
            get_threw = bool(e.code());
        }
        BOOST_TEST(get_threw);
        acc.close();
    }

    void testAcceptorClosedWaitCompletes()
    {
        io_context ioc(Backend);
        local_stream_acceptor acc(ioc);

        bool done = false;
        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await acc.wait(wait_type::read);
            BOOST_TEST(ec == std::errc::bad_file_descriptor);
            done = true;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(done);
    }

    void testAcceptorAcceptClosedThrows()
    {
        io_context ioc(Backend);
        local_stream_acceptor acc(ioc);
        local_stream_socket peer(ioc);

        // Accepts on a closed acceptor complete with
        // bad_file_descriptor instead of throwing.
        bool done = false;
        auto task = [&]() -> capy::task<> {
            auto [e1] = co_await acc.accept(peer);
            BOOST_TEST(e1 == std::errc::bad_file_descriptor);

            auto [e2, moved] = co_await acc.accept();
            BOOST_TEST(e2 == std::errc::bad_file_descriptor);
            done = true;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(done);
    }

    void testAcceptorReleaseClosedThrows()
    {
        io_context ioc(Backend);
        local_stream_acceptor acc(ioc);

        bool caught = false;
        try
        {
            std::ignore = acc.release();
        }
        catch (std::system_error const&)
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
        BOOST_TEST(!acc.open());
        auto ec = acc.bind(local_endpoint(path));
        BOOST_TEST_EQ(!ec, true);
        ec = acc.listen();
        BOOST_TEST_EQ(!ec, true);

        BOOST_TEST_EQ(acc.is_open(), true);
        auto h = acc.release();
        BOOST_TEST_EQ(acc.is_open(), false);

#if BOOST_COROSIO_HAS_IOCP
        ::closesocket(static_cast<SOCKET>(h));
#else
        if (static_cast<int>(h) >= 0)
            ::close(static_cast<int>(h));
#endif
    }

    void testAcceptorNativeHandle()
    {
        io_context ioc(Backend);
        local_stream_acceptor acc(ioc);

#if BOOST_COROSIO_HAS_IOCP
        auto const invalid = static_cast<native_handle_type>(~0ull);
#else
        auto const invalid = static_cast<native_handle_type>(-1);
#endif
        BOOST_TEST(acc.native_handle() == invalid);

        BOOST_TEST(!acc.open());
        BOOST_TEST(acc.native_handle() != invalid);
        acc.close();
        BOOST_TEST(acc.native_handle() == invalid);
    }

    // Drive one connect + accept + byte exchange through `acc`, which
    // must already be listening on `path`. Runs `ioc` to completion;
    // the connecting peer is spawned after the acceptor so the accept
    // is parked before the connect lands.
    bool acceptOneThroughLocal(
        io_context& ioc, local_stream_acceptor& acc, std::string const& path)
    {
        local_stream_socket peer(ioc);
        local_stream_socket client(ioc);
        bool done = false;

        auto server = [&]() -> capy::task<> {
            auto [aec] = co_await acc.accept(peer);
            BOOST_TEST(!aec);
            char in[8];
            auto [rec, rn] =
                co_await peer.read_some(capy::mutable_buffer(in, sizeof(in)));
            BOOST_TEST(!rec);
            done = (rn == 4);
        };
        auto sender = [&]() -> capy::task<> {
            auto [cec] = co_await client.connect(local_endpoint(path));
            BOOST_TEST(!cec);
            char const out[] = "ping";
            [[maybe_unused]] auto [wec, wn] =
                co_await client.write_some(capy::const_buffer(out, 4));
            BOOST_TEST(!wec);
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(server());
        capy::run_async(ex)(sender());
        ioc.run();
        return done;
    }

    // Adopting over a listening acceptor must retire the accept
    // machinery for the descriptor being replaced, not leave it
    // aliased onto the newly adopted one.
    void testAcceptorAssignOverListening()
    {
        io_context ioc(Backend);
        test::temp_socket_dir held_dir;
        test::temp_socket_dir adopted_dir;

        local_stream_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        auto ec = acc.bind(local_endpoint(held_dir.path()));
        BOOST_TEST_EQ(!ec, true);
        ec = acc.listen();
        BOOST_TEST_EQ(!ec, true);

        // Source the replacement descriptor from a second acceptor.
        local_stream_acceptor donor(ioc);
        BOOST_TEST(!donor.open());
        ec = donor.bind(local_endpoint(adopted_dir.path()));
        BOOST_TEST_EQ(!ec, true);
        ec = donor.listen();
        BOOST_TEST_EQ(!ec, true);
        auto h = donor.release();

        BOOST_TEST(!acc.assign(h));
        BOOST_TEST_EQ(acc.is_open(), true);
        BOOST_TEST(acc.native_handle() == h);
        BOOST_TEST_EQ(acc.local_endpoint().path(), adopted_dir.path());

        BOOST_TEST(acceptOneThroughLocal(ioc, acc, adopted_dir.path()));
    }

    // release() then assign() on the SAME object: the released
    // descriptor's accept machinery must be retired before the adopted
    // one is armed.
    void testAcceptorAssignAfterRelease()
    {
        io_context ioc(Backend);
        test::temp_socket_dir first_dir;
        test::temp_socket_dir second_dir;

        local_stream_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        auto ec = acc.bind(local_endpoint(first_dir.path()));
        BOOST_TEST_EQ(!ec, true);
        ec = acc.listen();
        BOOST_TEST_EQ(!ec, true);

        auto released = acc.release();
        BOOST_TEST_EQ(acc.is_open(), false);
#if BOOST_COROSIO_HAS_IOCP
        ::closesocket(static_cast<SOCKET>(released));
#else
        if (static_cast<int>(released) >= 0)
            ::close(static_cast<int>(released));
#endif

        local_stream_acceptor donor(ioc);
        BOOST_TEST(!donor.open());
        ec = donor.bind(local_endpoint(second_dir.path()));
        BOOST_TEST_EQ(!ec, true);
        ec = donor.listen();
        BOOST_TEST_EQ(!ec, true);

        BOOST_TEST(!acc.assign(donor.release()));
        BOOST_TEST_EQ(acc.is_open(), true);
        BOOST_TEST_EQ(acc.local_endpoint().path(), second_dir.path());

        BOOST_TEST(acceptOneThroughLocal(ioc, acc, second_dir.path()));
    }

    // A released listener round-trips into a fresh acceptor and keeps
    // serving connections.
    void testAcceptorAssignFromRelease()
    {
        io_context ioc(Backend);
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        local_stream_acceptor first(ioc);
        BOOST_TEST(!first.open());
        auto ec = first.bind(local_endpoint(path));
        BOOST_TEST_EQ(!ec, true);
        ec = first.listen();
        BOOST_TEST_EQ(!ec, true);

        auto h = first.release();
        BOOST_TEST_EQ(first.is_open(), false);

        local_stream_acceptor acc(ioc);
        BOOST_TEST(!acc.assign(h));
        BOOST_TEST_EQ(acc.is_open(), true);
        BOOST_TEST(acc.native_handle() == h);
        BOOST_TEST_EQ(acc.local_endpoint().path(), path);

        local_stream_socket peer(ioc);
        local_stream_socket client(ioc);
        auto ex   = ioc.get_executor();
        bool done = false;

        auto server = [&]() -> capy::task<> {
            auto [aec] = co_await acc.accept(peer);
            BOOST_TEST(!aec);
            char in[8];
            auto [rec, rn] =
                co_await peer.read_some(capy::mutable_buffer(in, sizeof(in)));
            BOOST_TEST(!rec);
            done = (rn == 4);
        };
        auto sender = [&]() -> capy::task<> {
            auto [cec] = co_await client.connect(local_endpoint(path));
            BOOST_TEST(!cec);
            char const out[] = "ping";
            [[maybe_unused]] auto [wec, wn] =
                co_await client.write_some(capy::const_buffer(out, 4));
            BOOST_TEST(!wec);
        };

        capy::run_async(ex)(server());
        capy::run_async(ex)(sender());
        ioc.run();
        BOOST_TEST(done);
    }

    void testAcceptorLocalEndpoint()
    {
        io_context ioc(Backend);
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        local_stream_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
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
            [[maybe_unused]] local_endpoint ep(too_long);
        }
        catch (std::system_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testEndpointTooLongPrecheck()
    {
        // Runtime-derived paths use the documented pre-check
        // instead of a non-throwing parse.
        std::string too_long(local_endpoint::max_path_length + 1, 'x');
        BOOST_TEST(too_long.size() > local_endpoint::max_path_length);

        local_endpoint ok("/tmp/ok");
        BOOST_TEST_EQ(ok.path(), std::string_view("/tmp/ok"));
    }

    void testEndpointMaxPathLength()
    {
        // Exactly at the limit should succeed.
        std::string at_limit(local_endpoint::max_path_length, 'a');
        local_endpoint ep(at_limit);
        BOOST_TEST_EQ(ep.path().size(), local_endpoint::max_path_length);
    }

    // local_endpoint is a value type: constructing and classifying an
    // abstract address needs no kernel support, so this runs on every
    // platform even though only Linux can bind one.
    void testAbstractEndpoint()
    {
        std::string abs_path(1, '\0');
        abs_path += "corosio_test_abstract_endpoint";
        local_endpoint ep(abs_path);
        BOOST_TEST(ep.is_abstract());
        BOOST_TEST_EQ(ep.empty(), false);
    }

#ifdef _WIN32
    // Windows AF_UNIX has no abstract namespace; bind must refuse the
    // endpoint instead of silently binding something else.
    void testAbstractBindRejected()
    {
        io_context ioc(Backend);
        std::string abs_path(1, '\0');
        abs_path += "corosio_test_abstract_bind";

        local_stream_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        auto ec = acc.bind(local_endpoint(abs_path));
        BOOST_TEST(ec == std::errc::operation_not_supported);
    }
#endif

    void testAssignSelfRejected()
    {
        io_context ioc(Backend);
        local_stream_socket s(ioc);
        BOOST_TEST(!s.open());
        BOOST_TEST(
            s.assign(s.native_handle()) ==
            std::make_error_code(std::errc::invalid_argument));
        BOOST_TEST(s.is_open());
    }

    void testAcceptorAssignSelfAndWrongType()
    {
        io_context ioc(Backend);
        local_stream_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(
            acc.assign(acc.native_handle()) ==
            std::make_error_code(std::errc::invalid_argument));
        BOOST_TEST(acc.is_open());

#if BOOST_COROSIO_POSIX
        // A datagram fd is not a listenable stream socket. Windows
        // AF_UNIX has no datagram sockets, so the probe is POSIX-only.
        local_datagram_socket d(ioc);
        BOOST_TEST(!d.open());
        auto dfd = d.release();
        BOOST_TEST(!!acc.assign(dfd));
        ::close(dfd);
#endif
    }


    void run()
    {
        testAssignSelfRejected();
        testAcceptorAssignSelfAndWrongType();

        testConstruction();
        testOpen();
        testMove();
        testMoveAssign();
        testCancelOnClosedSocket();
        testAcceptorConvenienceConstructor();
        testNativeHandleClosed();
        testEndpointsClosed();
        testConnectAccept();
        testMoveAccept();
        testReadWrite();
        testSocketPair();
        testEndpointsConnected();
        testShutdown();
#if BOOST_COROSIO_POSIX
        testAssignOverOpenAdopts();
        testAssignOverOpenRejectedKeepsSocket();
        testAssignBadFdThrows();
        testAssignRejectedFdStaysOpen();
#endif
#if BOOST_COROSIO_HAS_EPOLL
        testAssignDuplicateFdErrors();
#endif
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
        testDestroyWithQueuedRead();
        testDestroyWithQueuedConnect();
        testDestroyWithQueuedWait();
        testDestroyWithQueuedWrite();
#endif
        testAcceptorOnClosedNoOp();
        testAcceptorBindClosedThrows();
        testAcceptorListenClosedThrows();
        testOptionFailureThrows();
        testAcceptorClosedWaitCompletes();
        testAcceptorAcceptClosedThrows();
        testAcceptorReleaseClosedThrows();
        testAcceptorReleaseOpen();
        testAcceptorNativeHandle();
        testAcceptorAssignFromRelease();
        testAcceptorAssignOverListening();
        testAcceptorAssignAfterRelease();
        testAcceptorLocalEndpoint();
        testEndpointTooLongThrows();
        testEndpointTooLongPrecheck();
        testEndpointMaxPathLength();
        testAbstractEndpoint();
#ifdef _WIN32
        testAbstractBindRejected();
#endif
        testUnlinkExisting();
        testUnlinkNonexistent();
        testEndpointOrdering();
        testEndpointStreamOutput();
        testAvailable();
        testRelease();
        testReleaseCancelsPendingRead();
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
                std::ignore = co_await capy::write(s, capy::const_buffer(data, len));
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

    // release() hands ownership to the caller only after pending
    // operations have been cancelled, so no completion can resolve
    // against the descriptor number once the caller recycles it.
    void testReleaseCancelsPendingRead()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        local_stream_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        std::error_code read_ec;
        bool read_done = false;
        char buf[16];
#if BOOST_COROSIO_HAS_IOCP
        auto released = static_cast<native_handle_type>(~0ull);
#else
        auto released = static_cast<native_handle_type>(-1);
#endif

        auto reader = [&]() -> capy::task<> {
            [[maybe_unused]] auto [ec, n] = co_await s1.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            read_ec   = ec;
            read_done = true;
        };
        auto releaser = [&]() -> capy::task<> {
            released = s1.release();
            co_return;
        };

        // run_async runs inline to the first suspend, so spawning the
        // reader first is what guarantees the read is parked when
        // release() runs.
        capy::run_async(ex)(reader());
        capy::run_async(ex)(releaser());
        ioc.run();

        BOOST_TEST(read_done);
        BOOST_TEST(read_ec == capy::cond::canceled);
        BOOST_TEST_EQ(s1.is_open(), false);

#if BOOST_COROSIO_HAS_IOCP
        ::closesocket(static_cast<SOCKET>(released));
#else
        BOOST_TEST_EQ(released >= 0, true);
        ::close(released);
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

        // Abstract socket: formatting is value-type behavior, so it
        // is exercised on every platform.
        {
            std::string abs_path(1, '\0');
            abs_path += "test_name";
            std::ostringstream os;
            os << local_endpoint(abs_path);
            BOOST_TEST_EQ(os.str(), std::string("[abstract:test_name]"));
        }
    }
};

COROSIO_BACKEND_TESTS(
    local_stream_socket_test, "boost.corosio.local_stream_socket")

} // namespace boost::corosio
