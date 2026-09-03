//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// IOCP paths no other suite drives: per-op cancellation hooks for the
// write/connect/wait directions, teardown with overlapped ops still in
// flight, assign validation, and the receive-direction shutdowns.

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/random_access_file.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/udp.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/wait_type.hpp>

#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <boost/corosio/test/socket_pair.hpp>
#include <boost/corosio/test/temp_path.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

namespace {

// A connected local-stream pair built through an acceptor, mirroring
// make_socket_pair; must not be called from inside a coroutine (it
// runs the context to completion).
std::pair<local_stream_socket, local_stream_socket>
make_local_pair(io_context& ioc, test::temp_socket_dir const& tmp)
{
    local_stream_acceptor acc(ioc);
    if (acc.open())
        throw std::runtime_error("acceptor open");
    if (acc.bind(local_endpoint(tmp.path())))
        throw std::runtime_error("acceptor bind");
    if (acc.listen())
        throw std::runtime_error("acceptor listen");

    local_stream_socket client(ioc), server(ioc);
    if (client.open())
        throw std::runtime_error("client open");

    auto ex        = ioc.get_executor();
    auto connector = [](local_stream_socket& c,
                         corosio::local_endpoint ep) -> capy::task<> {
        auto [ec] = co_await c.connect(ep);
        BOOST_TEST(!ec);
    };
    auto accepter = [](local_stream_acceptor& a,
                        local_stream_socket& s) -> capy::task<> {
        auto [ec] = co_await a.accept(s);
        BOOST_TEST(!ec);
    };
    capy::run_async(ex)(connector(client, local_endpoint(tmp.path())));
    capy::run_async(ex)(accepter(acc, server));
    ioc.run();
    ioc.restart();
    acc.close();
    return {std::move(server), std::move(client)};
}

struct temp_file
{
    std::filesystem::path path;

    explicit temp_file(std::string_view contents)
    {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
            ("corosio_iocp_paths_" + std::to_string(counter++));
        std::ofstream(path) << contents;
    }

    ~temp_file()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    temp_file(temp_file const&)            = delete;
    temp_file& operator=(temp_file const&) = delete;
};

} // namespace

struct iocp_paths_test
{
    void testStopCancelsLocalStreamOps()
    {
        io_context ioc(iocp);
        auto ex = ioc.get_executor();
        test::temp_socket_dir tmp;
        auto [s1, s2] = make_local_pair(ioc, tmp);

        // SO_SNDBUF of zero makes every overlapped send pend until the
        // peer reads, so the write is reliably in flight when the stop
        // arrives.
        BOOST_TEST_NO_THROW(
            s1.set_option(socket_option::send_buffer_size(0)));

        std::stop_source ss;
        char big[65536] = {};
        char buf[8];
        std::error_code wec, wtec, rec;
        int done    = 0;
        auto writer = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.write_some(capy::const_buffer(big, sizeof(big)));
            std::ignore = n;
            wec         = ec;
            ++done;
        };
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::read);
            wtec      = ec;
            ++done;
        };
        auto reader = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            std::ignore = n;
            rec         = ec;
            ++done;
        };
        auto stopper = [&]() -> capy::task<> {
            ss.request_stop();
            co_return;
        };
        capy::run_async(ex, ss.get_token())(writer());
        capy::run_async(ex, ss.get_token())(waiter());
        capy::run_async(ex, ss.get_token())(reader());
        capy::run_async(ex)(stopper());
        ioc.run();

        BOOST_TEST_EQ(done, 3);
        BOOST_TEST(wec == capy::cond::canceled);
        BOOST_TEST(wtec == capy::cond::canceled);
        BOOST_TEST(rec == capy::cond::canceled);
    }

    void testStopCancelsAcceptorWaits()
    {
        io_context ioc(iocp);
        auto ex = ioc.get_executor();

        tcp_acceptor tacc(ioc);
        BOOST_TEST(!tacc.open(tcp::v4()));
        BOOST_TEST(!tacc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!tacc.listen());

        test::temp_socket_dir tmp;
        local_stream_acceptor lacc(ioc);
        BOOST_TEST(!lacc.open());
        BOOST_TEST(!lacc.bind(local_endpoint(tmp.path())));
        BOOST_TEST(!lacc.listen());

        std::stop_source ss;
        std::error_code tec, lec;
        int done     = 0;
        auto twaiter = [&]() -> capy::task<> {
            auto [ec] = co_await tacc.wait(wait_type::read);
            tec       = ec;
            ++done;
        };
        auto lwaiter = [&]() -> capy::task<> {
            auto [ec] = co_await lacc.wait(wait_type::read);
            lec       = ec;
            ++done;
        };
        auto stopper = [&]() -> capy::task<> {
            ss.request_stop();
            co_return;
        };
        capy::run_async(ex, ss.get_token())(twaiter());
        capy::run_async(ex, ss.get_token())(lwaiter());
        capy::run_async(ex)(stopper());
        ioc.run();

        BOOST_TEST_EQ(done, 2);
        BOOST_TEST(tec == capy::cond::canceled);
        BOOST_TEST(lec == capy::cond::canceled);
    }

    void testAssignValidation()
    {
        io_context ioc(iocp);
        auto const invalid = static_cast<native_handle_type>(~0ull);

        tcp_socket t(ioc);
        BOOST_TEST(!t.open(tcp::v4()));
        BOOST_TEST(!!t.assign(invalid));
        BOOST_TEST(
            t.assign(t.native_handle()) ==
            std::make_error_code(std::errc::invalid_argument));
        BOOST_TEST(t.is_open());

        // A datagram socket is the wrong type for a TCP stream slot.
        udp_socket u(ioc);
        BOOST_TEST(!u.open(udp::v4()));
        auto ufd = u.release();
        BOOST_TEST(!!t.assign(ufd));
        ::closesocket(static_cast<SOCKET>(ufd));

        udp_socket u2(ioc);
        BOOST_TEST(!u2.open(udp::v4()));
        BOOST_TEST(!!u2.assign(invalid));
        BOOST_TEST(
            u2.assign(u2.native_handle()) ==
            std::make_error_code(std::errc::invalid_argument));

        // An AF_INET socket cannot back an AF_UNIX acceptor.
        test::temp_socket_dir tmp;
        local_stream_acceptor lacc(ioc);
        BOOST_TEST(!lacc.open());
        tcp_socket t2(ioc);
        BOOST_TEST(!t2.open(tcp::v4()));
        auto tfd = t2.release();
        BOOST_TEST(!!lacc.assign(tfd));
        ::closesocket(static_cast<SOCKET>(tfd));

        BOOST_TEST(
            lacc.assign(lacc.native_handle()) ==
            std::make_error_code(std::errc::invalid_argument));
    }

    void testShutdownReceiveVariants()
    {
        io_context ioc(iocp);
        auto ex = ioc.get_executor();

        test::temp_socket_dir tmp;
        auto [l1, l2] = make_local_pair(ioc, tmp);
        BOOST_TEST(!l1.shutdown(shutdown_receive));

        udp_socket u1(ioc), u2(ioc);
        BOOST_TEST(!u1.open(udp::v4()));
        BOOST_TEST(!u2.open(udp::v4()));
        BOOST_TEST(!u1.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!u2.bind(endpoint(ipv4_address::loopback(), 0)));

        bool ok   = false;
        auto task = [&]() -> capy::task<> {
            auto [cec] = co_await u1.connect(u2.local_endpoint());
            if (!cec && !u1.shutdown(shutdown_receive))
                ok = true;
        };
        capy::run_async(ex)(task());
        ioc.run();
        BOOST_TEST(ok);
    }

    void testZeroLengthUdpReceive()
    {
        io_context ioc(iocp);
        auto ex = ioc.get_executor();

        udp_socket u1(ioc), u2(ioc);
        BOOST_TEST(!u1.open(udp::v4()));
        BOOST_TEST(!u2.open(udp::v4()));
        BOOST_TEST(!u1.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!u2.bind(endpoint(ipv4_address::loopback(), 0)));

        bool done = false;
        std::error_code rec;
        std::size_t rn = 99;
        auto task = [&]() -> capy::task<> {
            auto [cec] = co_await u1.connect(u2.local_endpoint());
            std::ignore = cec;
            auto [ec, n] = co_await u1.recv(capy::mutable_buffer(nullptr, 0));
            rec  = ec;
            rn   = n;
            done = true;
        };
        capy::run_async(ex)(task());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(!rec);
        BOOST_TEST_EQ(rn, 0u);
    }

    void testResolverEmptyInputs()
    {
        io_context ioc(iocp);
        auto ex = ioc.get_executor();
        resolver r(ioc);

        bool done = false;
        std::error_code fec;
        auto task = [&]() -> capy::task<> {
            auto [ec, res] = co_await r.resolve("", "");
            std::ignore    = res;
            fec            = ec;
            done           = true;
        };
        capy::run_async(ex)(task());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(!!fec);
    }

    void testReleasedAcceptorAccessors()
    {
        io_context ioc(iocp);
        auto const invalid = static_cast<native_handle_type>(~0ull);

        test::temp_socket_dir tmp;
        local_stream_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(local_endpoint(tmp.path())));
        BOOST_TEST(!acc.listen());

        auto fd = acc.release();
        BOOST_TEST(fd != invalid);
        ::closesocket(static_cast<SOCKET>(fd));

        BOOST_TEST(acc.native_handle() == invalid);
        BOOST_TEST_THROWS(
            acc.set_option(socket_option::reuse_address(true)),
            std::system_error);
        BOOST_TEST_THROWS(
            std::ignore = acc.get_option<socket_option::reuse_address>(),
            std::system_error);
    }

    void testTruncateWithoutCreate()
    {
        temp_file tmp("some existing content");
        io_context ioc(iocp);
        random_access_file f(ioc);
        BOOST_TEST(
            !f.open(tmp.path, file_base::write_only | file_base::truncate));
        BOOST_TEST_EQ(f.size(), 0u);
        f.close();
    }

#if !COROSIO_TEST_HAS_ASAN
    // These abandon parked coroutine frames by design; see context.hpp.

    void testDestroyWithParkedSocketOps()
    {
        int resumed = 0;
        {
            io_context ioc(iocp);
            auto ex = ioc.get_executor();

            udp_socket u(ioc);
            std::ignore = u.open(udp::v4());
            std::ignore = u.bind(endpoint(ipv4_address::loopback(), 0));

            tcp_acceptor tacc(ioc);
            std::ignore = tacc.open(tcp::v4());
            std::ignore = tacc.bind(endpoint(ipv4_address::loopback(), 0));
            std::ignore = tacc.listen();

            char buf[8];
            endpoint src;
            auto urecv = [&]() -> capy::task<> {
                std::ignore = co_await u.recv_from(
                    capy::mutable_buffer(buf, sizeof(buf)), src);
                ++resumed;
            };
            auto uwait = [&]() -> capy::task<> {
                std::ignore = co_await u.wait(wait_type::read);
                ++resumed;
            };
            auto await_ = [&]() -> capy::task<> {
                std::ignore = co_await tacc.wait(wait_type::read);
                ++resumed;
            };
            capy::run_async(ex)(urecv());
            capy::run_async(ex)(uwait());
            capy::run_async(ex)(await_());
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
        }
        BOOST_TEST_EQ(resumed, 0);
    }

    void testDestroyWithParkedLocalOps()
    {
        int resumed = 0;
        test::temp_socket_dir tmp;
        {
            io_context ioc(iocp);
            auto ex       = ioc.get_executor();
            auto [s1, s2] = make_local_pair(ioc, tmp);

            test::temp_socket_dir tmp2;
            local_stream_acceptor lacc(ioc);
            std::ignore = lacc.open();
            std::ignore = lacc.bind(local_endpoint(tmp2.path()));
            std::ignore = lacc.listen();

            auto reader = [](local_stream_socket s, int& count)
                -> capy::task<> {
                char b[8];
                std::ignore =
                    co_await s.read_some(capy::mutable_buffer(b, sizeof(b)));
                ++count;
            }(std::move(s1), resumed);
            auto lwait = [&]() -> capy::task<> {
                std::ignore = co_await lacc.wait(wait_type::read);
                ++resumed;
            };
            capy::run_async(ex)(std::move(reader));
            capy::run_async(ex)(lwait());
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
        }
        BOOST_TEST_EQ(resumed, 0);
    }
#endif // !COROSIO_TEST_HAS_ASAN

    void run()
    {
        testStopCancelsLocalStreamOps();
        testStopCancelsAcceptorWaits();
        testAssignValidation();
        testShutdownReceiveVariants();
        testZeroLengthUdpReceive();
        testResolverEmptyInputs();
        testReleasedAcceptorAccessors();
        testTruncateWithoutCreate();
#if !COROSIO_TEST_HAS_ASAN
        testDestroyWithParkedSocketOps();
        testDestroyWithParkedLocalOps();
#endif
    }
};

TEST_SUITE(iocp_paths_test, "boost.corosio.iocp_paths");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP
