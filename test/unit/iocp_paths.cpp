//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
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

#include <boost/corosio/error.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/random_access_file.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/family.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/wait_type.hpp>

#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <boost/corosio/test/socket_pair.hpp>
#include "temp_path.hpp"

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

using test::temp_file;

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
        BOOST_TEST_NO_THROW(s1.set_option(socket_option::send_buffer_size(0)));

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
        BOOST_TEST(!tacc.open(family::v4));
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

        // Validation runs only on a closed object; an open one reports
        // already_open first.
        tcp_socket t(ioc);
        BOOST_TEST(!!t.assign(invalid));
        BOOST_TEST(!t.is_open());

        // A datagram socket is the wrong type for a TCP stream slot.
        udp_socket u(ioc);
        BOOST_TEST(!u.open(family::v4));
        auto ufd = u.release();
        BOOST_TEST(!!t.assign(ufd));
        BOOST_TEST(!t.is_open());
        ::closesocket(static_cast<SOCKET>(ufd));

        BOOST_TEST(!t.open(family::v4));
        BOOST_TEST(t.assign(t.native_handle()) == error::already_open);
        BOOST_TEST(t.is_open());

        udp_socket u2(ioc);
        BOOST_TEST(!!u2.assign(invalid));
        BOOST_TEST(!u2.is_open());
        BOOST_TEST(!u2.open(family::v4));
        BOOST_TEST(u2.assign(u2.native_handle()) == error::already_open);

        // An AF_INET socket cannot back an AF_UNIX acceptor.
        test::temp_socket_dir tmp;
        local_stream_acceptor lacc(ioc);
        tcp_socket t2(ioc);
        BOOST_TEST(!t2.open(family::v4));
        auto tfd = t2.release();
        BOOST_TEST(!!lacc.assign(tfd));
        BOOST_TEST(!lacc.is_open());
        ::closesocket(static_cast<SOCKET>(tfd));

        BOOST_TEST(!lacc.open());
        BOOST_TEST(lacc.assign(lacc.native_handle()) == error::already_open);
    }

    // A socket in skip-on-success mode queues no packet for a
    // synchronous success, so every adopting type must refuse it and
    // leave it with the caller.
    void testAssignRejectsSkipOnSuccessSockets()
    {
        io_context ioc(iocp);

        auto skipping = [](int af, int type, int proto) {
            SOCKET s = ::WSASocketW(
                af, type, proto, nullptr, 0, WSA_FLAG_OVERLAPPED);
            BOOST_TEST(s != INVALID_SOCKET);
            BOOST_TEST(::SetFileCompletionNotificationModes(
                reinterpret_cast<HANDLE>(s),
                FILE_SKIP_COMPLETION_PORT_ON_SUCCESS));
            return s;
        };
        auto refused = [](auto& obj, SOCKET s) {
            BOOST_TEST(
                obj.assign(static_cast<native_handle_type>(s)) ==
                std::errc::operation_not_supported);
            BOOST_TEST(!obj.is_open());
            BOOST_TEST(::closesocket(s) == 0);
        };

        tcp_socket t(ioc);
        refused(t, skipping(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        tcp_acceptor ta(ioc);
        refused(ta, skipping(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        udp_socket u(ioc);
        refused(u, skipping(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        local_stream_socket l(ioc);
        refused(l, skipping(AF_UNIX, SOCK_STREAM, 0));
        local_stream_acceptor la(ioc);
        refused(la, skipping(AF_UNIX, SOCK_STREAM, 0));
    }

    // A stop that lands while a wait is being registered with the wait
    // reactor must still end it: the reactor may already have dropped
    // the cancel as one for an op it never parked.
    void testStopRacingWaitRegistration()
    {
        io_context ioc(iocp, 2);
        udp_socket u(ioc);
        BOOST_TEST(!u.open(family::v4));
        BOOST_TEST(!u.bind(endpoint(ipv4_address::loopback(), 0)));

        std::atomic<bool> quit{false};
        std::vector<std::thread> runners;
        for (int i = 0; i < 2; ++i)
            runners.emplace_back([&] {
                while (!quit.load())
                    std::ignore = ioc.run_one_for(std::chrono::milliseconds(5));
            });

        int lost = 0;
        for (int i = 0; i < 500; ++i)
        {
            std::stop_source ss;
            std::atomic<bool> done{false};
            capy::run_async(ioc.get_executor(), ss.get_token())(
                [](udp_socket& u, std::atomic<bool>& d) -> capy::task<> {
                    std::ignore = co_await u.wait(wait_type::error);
                    d = true;
                }(u, done));
            ss.request_stop();
            // Bounded only to fail rather than hang.
            for (int k = 0; k < 400 && !done.load(); ++k)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!done.load())
            {
                ++lost;
                // The parked wait still refers to `done`: end it while
                // this iteration's locals are alive.
                u.close();
                for (int k = 0; k < 400 && !done.load(); ++k)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                break;
            }
        }
        BOOST_TEST_EQ(lost, 0);
        u.close();
        quit = true;
        for (auto& t : runners)
            t.join();
    }

    void testShutdownReceiveVariants()
    {
        io_context ioc(iocp);
        auto ex = ioc.get_executor();

        test::temp_socket_dir tmp;
        auto [l1, l2] = make_local_pair(ioc, tmp);
        BOOST_TEST(!l1.shutdown(shutdown_receive));

        udp_socket u1(ioc), u2(ioc);
        BOOST_TEST(!u1.open(family::v4));
        BOOST_TEST(!u2.open(family::v4));
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
        BOOST_TEST(!u1.open(family::v4));
        BOOST_TEST(!u2.open(family::v4));
        BOOST_TEST(!u1.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!u2.bind(endpoint(ipv4_address::loopback(), 0)));

        bool done = false;
        std::error_code rec;
        std::size_t rn = 99;
        auto task      = [&]() -> capy::task<> {
            auto [cec]   = co_await u1.connect(u2.local_endpoint());
            std::ignore  = cec;
            auto [ec, n] = co_await u1.recv(capy::mutable_buffer(nullptr, 0));
            rec          = ec;
            rn           = n;
            done         = true;
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
        temp_file tmp("corosio_iocp_", "some existing content");
        io_context ioc(iocp);
        random_access_file f(ioc);
        BOOST_TEST(
            !f.open(tmp.path, file_base::write_only | file_base::truncate));
        BOOST_TEST_EQ(f.size(), 0u);
        f.close();
    }

    // close() asks the wait reactor to drop the object's wait op even
    // when none was ever parked. Objects destroyed before the reactor
    // thread exists leave those asks queued; the first wait starts the
    // thread, which must not touch the dead ops (ASan catches it).
    void testStaleWaitCancelsFromDestroyedObjects()
    {
        io_context ioc(iocp);
        {
            tcp_acceptor a(ioc);
            std::ignore = a.open(family::v4);
            tcp_socket s(ioc);
            std::ignore = s.open(family::v4);
            udp_socket u(ioc);
            std::ignore = u.open(family::v4);
        }
        auto [s1, s2] = test::make_socket_pair(ioc);
        std::error_code ec = std::make_error_code(std::errc::io_error);
        auto waiter        = [&]() -> capy::task<> {
            auto [e] = co_await s1.wait(wait_type::write);
            ec       = e;
        };
        capy::run_async(ioc.get_executor())(waiter());
        ioc.run();
        BOOST_TEST(!ec);
    }

    // release() cancels each op and then tries to unbind the socket from
    // the port. Every cancelled op must still report through the port,
    // or the op pins its object and run() never returns. Windows keeps a
    // socket with I/O outstanding bound, so such a socket stays on this
    // context's port.
    void testReleaseWithKernelOpsInFlight()
    {
        io_context ioc(iocp);
        auto ex       = ioc.get_executor();
        auto [r1, r2] = test::make_socket_pair(ioc);
        test::temp_socket_dir tmp;
        auto [l1, l2] = make_local_pair(ioc, tmp);

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open(family::v4));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        tcp_socket peer(ioc);

        test::temp_socket_dir ltmp;
        local_stream_acceptor lacc(ioc);
        BOOST_TEST(!lacc.open());
        BOOST_TEST(!lacc.bind(local_endpoint(ltmp.path())));
        BOOST_TEST(!lacc.listen());
        local_stream_socket lpeer(ioc);

        // Loopback connects to a closed port retry for seconds on
        // Windows, so the connect stays in flight.
        std::uint16_t closed_port = 0;
        {
            tcp_acceptor probe(ioc);
            BOOST_TEST(!probe.open(family::v4));
            BOOST_TEST(!probe.bind(endpoint(ipv4_address::loopback(), 0)));
            closed_port = probe.local_endpoint().port();
        }
        tcp_socket conn(ioc);
        BOOST_TEST(!conn.open(family::v4));

        udp_socket u(ioc);
        BOOST_TEST(!u.open(family::v4));
        BOOST_TEST(!u.bind(endpoint(ipv4_address::loopback(), 0)));

        std::error_code ec[6];
        int done = 0;
        char rbuf[8], lbuf[8], ubuf[8];
        endpoint from;
        auto read_r = [&]() -> capy::task<> {
            auto [e, n] =
                co_await r1.read_some(capy::mutable_buffer(rbuf, sizeof(rbuf)));
            std::ignore = n;
            ec[0]       = e;
            ++done;
        };
        auto accept_t = [&]() -> capy::task<> {
            auto [e] = co_await acc.accept(peer);
            ec[1]    = e;
            ++done;
        };
        auto connect_t = [&]() -> capy::task<> {
            auto [e] = co_await conn.connect(
                endpoint(ipv4_address::loopback(), closed_port));
            ec[2] = e;
            ++done;
        };
        auto recv_u = [&]() -> capy::task<> {
            auto [e, n] = co_await u.recv_from(
                capy::mutable_buffer(ubuf, sizeof(ubuf)), from);
            std::ignore = n;
            ec[3]       = e;
            ++done;
        };
        auto read_l = [&]() -> capy::task<> {
            auto [e, n] =
                co_await l1.read_some(capy::mutable_buffer(lbuf, sizeof(lbuf)));
            std::ignore = n;
            ec[4]       = e;
            ++done;
        };
        auto accept_l = [&]() -> capy::task<> {
            auto [e] = co_await lacc.accept(lpeer);
            ec[5]    = e;
            ++done;
        };
        capy::run_async(ex)(read_r());
        capy::run_async(ex)(accept_t());
        capy::run_async(ex)(connect_t());
        capy::run_async(ex)(recv_u());
        capy::run_async(ex)(read_l());
        capy::run_async(ex)(accept_l());
        std::ignore = ioc.poll();
        ioc.restart();
        BOOST_TEST_EQ(done, 0);

        native_handle_type released[] = {
            r1.release(), acc.release(), conn.release(),
            u.release(),  l1.release(),  lacc.release()};

        std::ignore = ioc.run_for(std::chrono::seconds(30));
        BOOST_TEST_EQ(done, 6);
        for (auto const& e : ec)
            BOOST_TEST(e == capy::cond::canceled);

        for (auto h : released)
            ::closesocket(static_cast<SOCKET>(h));
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
            std::ignore = u.open(family::v4);
            std::ignore = u.bind(endpoint(ipv4_address::loopback(), 0));

            tcp_acceptor tacc(ioc);
            std::ignore = tacc.open(family::v4);
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

            auto reader = [](local_stream_socket s,
                             int& count) -> capy::task<> {
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

    void testDestroyWithParkedTcpStreamOps()
    {
        // Each socket has one wait slot, so the four ops need four
        // sockets: a read and a zero-byte-WSARecv read wait on one pair
        // (nothing is ever sent), and a write wait on a full send
        // buffer plus an error wait on a healthy socket, both parked in
        // the poll reactor, on the other.
        int resumed = 0;
        {
            io_context ioc(iocp);
            auto ex       = ioc.get_executor();
            auto [a1, a2] = test::make_socket_pair(ioc);
            auto [b1, b2] = test::make_socket_pair(ioc);

            // Fill b1's send buffer; b2 never reads.
            auto const raw = static_cast<SOCKET>(b1.native_handle());
            u_long nonblocking = 1;
            BOOST_TEST(::ioctlsocket(raw, FIONBIO, &nonblocking) == 0);
            std::vector<char> chunk(64 * 1024, 'x');
            while (::send(raw, chunk.data(), static_cast<int>(chunk.size()),
                       0) != SOCKET_ERROR)
            {
            }
            BOOST_TEST_EQ(::WSAGetLastError(), WSAEWOULDBLOCK);

            char buf[8];
            auto aread = [&]() -> capy::task<> {
                std::ignore =
                    co_await a1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
                ++resumed;
            };
            auto await_read = [&]() -> capy::task<> {
                std::ignore = co_await a2.wait(wait_type::read);
                ++resumed;
            };
            auto await_write = [&]() -> capy::task<> {
                std::ignore = co_await b1.wait(wait_type::write);
                ++resumed;
            };
            auto await_error = [&]() -> capy::task<> {
                std::ignore = co_await b2.wait(wait_type::error);
                ++resumed;
            };
            capy::run_async(ex)(aread());
            capy::run_async(ex)(await_read());
            capy::run_async(ex)(await_write());
            capy::run_async(ex)(await_error());
            for (int i = 0; i < 4; ++i)
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
        testAssignRejectsSkipOnSuccessSockets();
        testStopRacingWaitRegistration();
        testShutdownReceiveVariants();
        testZeroLengthUdpReceive();
        testResolverEmptyInputs();
        testReleasedAcceptorAccessors();
        testTruncateWithoutCreate();
        testStaleWaitCancelsFromDestroyedObjects();
        testReleaseWithKernelOpsInFlight();
#if !COROSIO_TEST_HAS_ASAN
        testDestroyWithParkedSocketOps();
        testDestroyWithParkedLocalOps();
        testDestroyWithParkedTcpStreamOps();
#endif
    }
};

TEST_SUITE(iocp_paths_test, "boost.corosio.iocp_paths");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP
