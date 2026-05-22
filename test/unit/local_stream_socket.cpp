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

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>
#include <boost/capy/concept/read_stream.hpp>
#include <boost/capy/concept/write_stream.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <compare>
#include <cstring>
#include <sstream>
#include <string>

#if BOOST_COROSIO_POSIX
#include <unistd.h>
#endif

#include "context.hpp"
#include "local_temp.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

// Verify local_stream_socket satisfies stream concepts

static_assert(capy::ReadStream<local_stream_socket>);
static_assert(capy::WriteStream<local_stream_socket>);

using test::make_temp_socket_path;
using test::cleanup_temp_socket;

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
        auto path = make_temp_socket_path();

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

        cleanup_temp_socket(path);

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
        auto path = make_temp_socket_path();

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

        cleanup_temp_socket(path);

        BOOST_TEST_EQ(accept_done, true);
        BOOST_TEST_EQ(!accept_ec, true);
        BOOST_TEST_EQ(server_open, true);
        BOOST_TEST_EQ(connect_done, true);
        BOOST_TEST_EQ(!connect_ec, true);
    }

#if BOOST_COROSIO_POSIX
    // Uses make_local_stream_pair, which is socketpair-based and POSIX-only.
    void testReadWrite()
    {
        io_context ioc(Backend);
        auto [s1, s2] = make_local_stream_pair(ioc);

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
        auto [s1, s2] = make_local_stream_pair(ioc);

        BOOST_TEST_EQ(s1.is_open(), true);
        BOOST_TEST_EQ(s2.is_open(), true);
    }
#endif // BOOST_COROSIO_POSIX

    void testUnlinkExisting()
    {
        io_context ioc(Backend);
        auto path = make_temp_socket_path();

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

        cleanup_temp_socket(path);
    }

    void testUnlinkNonexistent()
    {
        // unlink_existing on a path that doesn't exist should
        // succeed (unlink silently fails with ENOENT).
        io_context ioc(Backend);
        auto path = make_temp_socket_path();

        local_stream_acceptor acc(ioc);
        acc.open();
        auto ec = acc.bind(
            local_endpoint(path), bind_option::unlink_existing);
        BOOST_TEST_EQ(!ec, true);

        cleanup_temp_socket(path);
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

    void run()
    {
        testConstruction();
        testOpen();
        testMove();
        testConnectAccept();
        testMoveAccept();
#if BOOST_COROSIO_POSIX
        testReadWrite();
        testSocketPair();
#endif
        testUnlinkExisting();
        testUnlinkNonexistent();
        testEndpointOrdering();
        testEndpointStreamOutput();
#if BOOST_COROSIO_POSIX
        testAvailable();
        testRelease();
#endif
    }

#if BOOST_COROSIO_POSIX
    // Uses make_local_stream_pair, which is socketpair-based and POSIX-only.
    void testAvailable()
    {
        io_context ioc(Backend);
        auto [s1, s2] = make_local_stream_pair(ioc);

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
#endif // BOOST_COROSIO_POSIX

#if BOOST_COROSIO_POSIX
    // Exercises raw POSIX fd ops (::write, ::close) on the released
    // descriptor. The released-handle semantics are tested via the
    // platform helpers; skipped on Windows because the analogous
    // path needs send/closesocket and isn't yet factored.
    void testRelease()
    {
        io_context ioc(Backend);
        auto [s1, s2] = make_local_stream_pair(ioc);

        BOOST_TEST_EQ(s1.is_open(), true);

        int fd = s1.release();
        BOOST_TEST_EQ(fd >= 0, true);
        BOOST_TEST_EQ(s1.is_open(), false);

        // The released fd is still valid -- write through it
        char const msg[] = "released";
        BOOST_TEST_EQ(::write(fd, msg, std::strlen(msg)) > 0, true);
        ::close(fd);
    }
#endif

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
