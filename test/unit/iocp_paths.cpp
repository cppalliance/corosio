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
// flight, close with a second handle to the socket open, assign
// validation, and the receive-direction shutdowns.

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/delay.hpp>
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

#include <boost/corosio/native/detail/iocp/win_tcp_acceptor_service.hpp>
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
#include <future>
#include <iterator>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "context.hpp"
#include "must.hpp"
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
    if (acc.bind(test::must(make_local_endpoint(tmp.path()))))
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
    capy::run_async(ex)(
        connector(client, test::must(make_local_endpoint(tmp.path()))));
    capy::run_async(ex)(accepter(acc, server));
    ioc.run();
    ioc.restart();
    acc.close();
    return {std::move(server), std::move(client)};
}

// A second handle to the socket `h` names, as an inherited or
// duplicated handle would be, so closing `h` no longer closes the last
// one.
SOCKET
duplicate_socket(native_handle_type h)
{
    WSAPROTOCOL_INFOW info{};
    if (::WSADuplicateSocketW(
            static_cast<SOCKET>(h), ::GetCurrentProcessId(), &info) != 0)
        return INVALID_SOCKET;
    return ::WSASocketW(
        FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0,
        WSA_FLAG_OVERLAPPED);
}

// Send one byte on `s` past the library, as a peer outside it would.
int
send_byte(tcp_socket& s, char byte)
{
    return ::send(static_cast<SOCKET>(s.native_handle()), &byte, 1, 0);
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
        test::must_ok(s1.set_option(socket_option::send_buffer_size(0)));

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
        BOOST_TEST(!lacc.bind(test::must(make_local_endpoint(tmp.path()))));
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
        auto const invalid = detail::invalid_native_handle;

        // Validation runs only on a closed object; an open one reports
        // already_open first.
        tcp_socket t(ioc);
        BOOST_TEST(!!t.assign(invalid));
        BOOST_TEST(!t.is_open());

        // A datagram socket is the wrong type for a TCP stream slot.
        udp_socket u(ioc);
        BOOST_TEST(!u.open(family::v4));
        auto [ufd_ec, ufd] = u.release();
        BOOST_TEST(!ufd_ec);
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
        auto [tfd_ec, tfd] = t2.release();
        BOOST_TEST(!tfd_ec);
        BOOST_TEST(!!lacc.assign(tfd));
        BOOST_TEST(!lacc.is_open());
        ::closesocket(static_cast<SOCKET>(tfd));

        BOOST_TEST(!lacc.open());
        BOOST_TEST(lacc.assign(lacc.native_handle()) == error::already_open);
    }

    // A socket in skip-on-success mode queues no packet for a
    // synchronous success, so every adopting type that waits for one
    // must refuse it and leave it with the caller. tcp_socket and
    // tcp_acceptor take such results from the call and are covered
    // below.
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

        udp_socket u(ioc);
        refused(u, skipping(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        local_stream_socket l(ioc);
        refused(l, skipping(AF_UNIX, SOCK_STREAM, 0));
        local_stream_acceptor la(ioc);
        refused(la, skipping(AF_UNIX, SOCK_STREAM, 0));
    }

    // Exchange a few bytes each way and say whether both arrived.
    static bool exchanges(io_context& ioc, tcp_socket& a, tcp_socket& b)
    {
        int done  = 0;
        auto pass = [&](tcp_socket& from, tcp_socket& to) -> capy::task<> {
            for (int i = 0; i < 40; ++i)
            {
                char out[4]    = {'p', 'i', 'n', 'g'};
                char in[4]     = {};
                auto [wec, wn] = co_await from.write_some(
                    capy::const_buffer(out, sizeof(out)));
                if (wec || wn != sizeof(out))
                    co_return;
                std::size_t got = 0;
                while (got < sizeof(in))
                {
                    auto [rec, rn] = co_await to.read_some(
                        capy::mutable_buffer(in + got, sizeof(in) - got));
                    if (rec)
                        co_return;
                    got += rn;
                }
                if (std::string(in, sizeof(in)) != "ping")
                    co_return;
            }
            ++done;
        };
        capy::run_async(ioc.get_executor())(pass(a, b));
        capy::run_async(ioc.get_executor())(pass(b, a));
        ioc.run();
        ioc.restart();
        return done == 2;
    }

    // A TCP socket already in skip-on-success mode is adopted as it
    // is, and its operations complete whether the kernel finishes
    // them during the call or later.
    void testAssignAdoptsSkipOnSuccessTcpSocket()
    {
        io_context ioc(iocp);
        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open(family::v4));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());

        SOCKET raw = ::WSASocketW(
            AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
        BOOST_TEST(raw != INVALID_SOCKET);
        BOOST_TEST(
            ::SetFileCompletionNotificationModes(
                reinterpret_cast<HANDLE>(raw),
                FILE_SKIP_COMPLETION_PORT_ON_SUCCESS));
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port        = htons(acc.local_endpoint().port());
        // The listen backlog completes the handshake, so this returns
        // before anything accepts.
        BOOST_TEST(
            ::connect(raw, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);

        tcp_socket client(ioc);
        BOOST_TEST(!client.assign(static_cast<native_handle_type>(raw)));
        BOOST_TEST(client.is_open());

        tcp_socket server(ioc);
        std::error_code aec = std::make_error_code(std::errc::io_error);
        auto accept_t       = [&]() -> capy::task<> {
            auto [e] = co_await acc.accept(server);
            aec      = e;
        };
        capy::run_async(ioc.get_executor())(accept_t());
        ioc.run();
        ioc.restart();
        BOOST_TEST(!aec);

        BOOST_TEST(exchanges(ioc, client, server));
    }

    // Suspend by posting the coroutine's own continuation.
    struct post_awaitable
    {
        capy::continuation* cont;

        bool await_ready() const noexcept
        {
            return false;
        }

        void await_suspend(
            std::coroutine_handle<> h, capy::await_context const* env) noexcept
        {
            cont->h = h;
            env->executor.post(*cont);
        }

        void await_resume() const noexcept {}
    };

    // An expired timer's handler is queued by the run loop itself, not
    // by a handler. A coroutine it resumes that then posts and gives
    // the timer's work back leaves only the post outstanding; the
    // loop must keep running for it and for what it starts.
    void testPostAfterTimerKeepsRunning()
    {
        io_context ioc(iocp);
        capy::continuation cont{};
        bool done = false;
        auto body = [&]() -> capy::task<> {
            std::ignore = co_await delay(std::chrono::milliseconds(1));
            co_await post_awaitable{&cont};
            std::ignore = co_await delay(std::chrono::milliseconds(20));
            done        = true;
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST(done);
    }

    // A handler that restarts the context after a stop keeps the run
    // going, even when the stop was dequeued in the same batch.
    void testRestartAfterStopKeepsRunning()
    {
        io_context ioc(iocp);
        bool done = false;
        auto body = [&]() -> capy::task<> {
            ioc.restart();
            std::ignore = co_await delay(std::chrono::milliseconds(20));
            done        = true;
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.stop();
        ioc.run();
        BOOST_TEST(done);
    }

    // A handler may block its thread until other work of the context
    // has run, as long as another loop is there to run it. A loop that
    // starts while the first is inside such a handler must reach what
    // the first has queued off the port: the packets dequeued in the
    // same batch as the handler, and what the handler posted.
    void testSecondLoopRunsWorkBehindBlockedHandler()
    {
        for (bool own_post : {false, true})
        {
            io_context ioc(iocp);
            auto ex = ioc.get_executor();
            capy::continuation cont{};
            std::promise<void> started;
            std::promise<void> ran;
            bool reached = false;
            auto second  = [&]() -> capy::task<> {
                // Started from the first handler, it runs inline up
                // to here and leaves the rest as that handler's post.
                if (own_post)
                    co_await post_awaitable{&cont};
                ran.set_value();
            };
            auto first = [&]() -> capy::task<> {
                if (own_post)
                    capy::run_async(ex)(second());
                started.set_value();
                reached = ran.get_future().wait_for(std::chrono::seconds(
                              30)) == std::future_status::ready;
                co_return;
            };
            // A stopped context sends a post through the port, so
            // both are there before a loop runs, and the first loop
            // takes them in one batch.
            ioc.stop();
            capy::run_async(ex)(first());
            if (!own_post)
                capy::run_async(ex)(second());
            ioc.restart();

            std::thread a([&] { ioc.run(); });
            started.get_future().wait();
            std::thread b([&] { ioc.run(); });
            a.join();
            b.join();
            BOOST_TEST(reached);
        }
    }

    // Work a handler posts while the context is stopped waits behind
    // the stop. Once the handler has restarted the context it runs
    // after what the handler posted ahead of the stop, and ahead of
    // what it posts after the restart.
    void testPostsAcrossRestartKeepTheirOrder()
    {
        for (bool lockless : {false, true})
        {
            io_context_options opts;
            if (lockless)
                opts.locking = locking_mode::unsafe_io;
            io_context ioc(iocp, opts);
            auto ex = ioc.get_executor();
            capy::continuation c0{};
            capy::continuation c1{};
            capy::continuation c2{};
            std::vector<int> order;
            auto step = [&](capy::continuation& c, int n) -> capy::task<> {
                co_await post_awaitable{&c};
                order.push_back(n);
            };
            auto body = [&]() -> capy::task<> {
                capy::run_async(ex)(step(c0, 0));
                ioc.stop();
                capy::run_async(ex)(step(c1, 1));
                ioc.restart();
                capy::run_async(ex)(step(c2, 2));
                co_return;
            };
            capy::run_async(ex)(body());
            ioc.run();
            BOOST_TEST(order == (std::vector<int>{0, 1, 2}));
        }
    }

    // A post that is not a lone run()'s goes through the port while
    // the context is stopped, and is queued without a packet once it
    // runs again. One made after the restart must not overtake one
    // made before it, which may still be in the port.
    void testPostsThroughThePortKeepTheirOrder()
    {
        for (bool lockless : {false, true})
        {
            io_context_options opts;
            if (lockless)
                opts.locking = locking_mode::unsafe_io;

            // Posted from outside the loop.
            {
                io_context ioc(iocp, opts);
                auto ex = ioc.get_executor();
                std::vector<int> order;
                auto step = [&](int n) -> capy::task<> {
                    order.push_back(n);
                    co_return;
                };
                ioc.stop();
                capy::run_async(ex)(step(0));
                ioc.restart();
                capy::run_async(ex)(step(1));
                ioc.run();
                BOOST_TEST(order == (std::vector<int>{0, 1}));
            }

            // Posted by a handler that a single-handler call runs.
            {
                io_context ioc(iocp, opts);
                auto ex = ioc.get_executor();
                capy::continuation c0{};
                capy::continuation c1{};
                std::vector<int> order;
                auto step = [&](capy::continuation& c, int n) -> capy::task<> {
                    co_await post_awaitable{&c};
                    order.push_back(n);
                };
                auto body = [&]() -> capy::task<> {
                    ioc.stop();
                    capy::run_async(ex)(step(c0, 0));
                    ioc.restart();
                    capy::run_async(ex)(step(c1, 1));
                    co_return;
                };
                capy::run_async(ex)(body());
                while (order.size() < 2 && ioc.run_one() != 0)
                {
                }
                BOOST_TEST(order == (std::vector<int>{0, 1}));
            }

            // Posted by a handler of run(), which finds its work
            // queued without a packet and runs it before it looks at
            // the port, where an earlier post still is.
            {
                io_context ioc(iocp, opts);
                auto ex = ioc.get_executor();
                capy::continuation c{};
                std::vector<int> order;
                auto first = [&]() -> capy::task<> {
                    order.push_back(0);
                    co_await post_awaitable{&c};
                    order.push_back(2);
                };
                auto second = [&]() -> capy::task<> {
                    order.push_back(1);
                    co_return;
                };
                capy::run_async(ex)(first());
                ioc.stop();
                capy::run_async(ex)(second());
                ioc.restart();
                ioc.run();
                BOOST_TEST(order == (std::vector<int>{0, 1, 2}));
            }

            // Posted by a handler of run() that has stopped the
            // context again, with an earlier post still in the port.
            // Held behind the stop, it would be queued again at the
            // restart and run before the port is looked at.
            {
                io_context ioc(iocp, opts);
                auto ex = ioc.get_executor();
                capy::continuation c{};
                std::vector<int> order;
                auto first = [&]() -> capy::task<> {
                    ioc.stop();
                    co_await post_awaitable{&c};
                    order.push_back(1);
                };
                auto second = [&]() -> capy::task<> {
                    order.push_back(0);
                    co_return;
                };
                capy::run_async(ex)(first());
                ioc.stop();
                capy::run_async(ex)(second());
                ioc.restart();
                // Runs `first` as far as its post, and ends on the stop.
                ioc.run();
                BOOST_TEST(order.empty());
                ioc.restart();
                ioc.run();
                BOOST_TEST(order == (std::vector<int>{0, 1}));
            }
        }
    }

    // Work posted from a thread that is not running the loop takes no
    // packet. It still runs in the order it was posted in.
    void testOutsidePostsRunInOrder()
    {
        io_context ioc(iocp);
        auto ex              = ioc.get_executor();
        constexpr int count = 1000;
        std::vector<int> order;
        order.reserve(count);
        auto step = [&](int i) -> capy::task<> {
            order.push_back(i);
            co_return;
        };
        for (int i = 0; i < count; ++i)
            capy::run_async(ex)(step(i));
        ioc.run();
        bool in_order = order.size() == static_cast<std::size_t>(count);
        for (int i = 0; in_order && i < count; ++i)
            in_order = order[static_cast<std::size_t>(i)] == i;
        BOOST_TEST(in_order);
    }

    // A loop blocked in the port learns of such a post only from a
    // wakeup, and each post finds the loops blocked again.
    void testOutsidePostsWakeBlockedLoops()
    {
        io_context ioc(iocp, 2);
        auto ex = ioc.get_executor();
        std::stop_source ss;
        // Keeps the loops running, and in the port, between posts.
        capy::run_async(ex, ss.get_token())([]() -> capy::task<> {
            std::ignore = co_await delay(std::chrono::seconds(60));
        }());
        std::vector<std::thread> loops;
        for (int i = 0; i < 2; ++i)
            loops.emplace_back([&] { ioc.run(); });

        std::atomic<int> ran{0};
        int lost = 0;
        for (int i = 0; i < 200 && lost == 0; ++i)
        {
            capy::run_async(ex)([](std::atomic<int>& n) -> capy::task<> {
                ++n;
                co_return;
            }(ran));
            // Bounded only to fail rather than hang.
            for (int k = 0; k < 2000 && ran.load() != i + 1; ++k)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (ran.load() != i + 1)
                ++lost;
        }
        BOOST_TEST_EQ(lost, 0);
        ss.request_stop();
        ioc.stop();
        for (auto& t : loops)
            t.join();
    }

    // A thread that runs no loop posts the work that ends the run,
    // and the context is destroyed as soon as the run returns. That
    // thread may still be a few instructions short of leaving its
    // post, and the destructor waits for it.
    void testContextDestroyedBehindOutsidePost()
    {
        for (int i = 0; i < 200; ++i)
        {
            std::atomic<bool> spinning{false};
            std::thread poster;
            {
                io_context ioc(iocp);
                auto ex   = ioc.get_executor();
                bool quit = false;
                capy::continuation cont{};
                // Keeps the loop out of the port, so that it comes by
                // the posted work with no wakeup to wait for.
                auto spinner = [&]() -> capy::task<> {
                    while (!quit)
                    {
                        spinning.store(true);
                        co_await post_awaitable{&cont};
                    }
                };
                capy::run_async(ex)(spinner());
                poster = std::thread([&spinning, &quit, ex] {
                    while (!spinning.load())
                        std::this_thread::yield();
                    capy::run_async(ex)([](bool& q) -> capy::task<> {
                        q = true;
                        co_return;
                    }(quit));
                });
                ioc.run();
                BOOST_TEST(quit);
            }
            poster.join();
        }
    }

    // A single-handler call takes queued work one entry a call. A
    // completion waiting in the port must get its turn all the same,
    // although the queue never runs empty.
    void testRunOneReachesPortBehindQueuedWork()
    {
        io_context ioc(iocp);
        auto ex       = ioc.get_executor();
        auto [a, b]   = test::must(test::make_socket_pair(ioc));
        bool got      = false;
        bool quit     = false;
        char buf[1]   = {};
        auto reader   = [&]() -> capy::task<> {
            std::ignore =
                co_await a.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            got = true;
        };
        capy::continuation cont{};
        auto spinner = [&]() -> capy::task<> {
            // Leaves one entry queued behind itself every time it runs.
            while (!quit)
                co_await post_awaitable{&cont};
        };
        capy::run_async(ex)(reader());
        capy::run_async(ex)(spinner());
        // The read reaches the kernel, and the spinner its first post.
        BOOST_TEST_EQ(ioc.run_one(), 1u);
        BOOST_TEST_EQ(ioc.run_one(), 1u);
        BOOST_TEST(!got);

        // Complete the read. Its packet reaches the port at once or
        // a moment later, and from then on every call comes by it.
        // Bounded only to fail rather than hang.
        BOOST_TEST_EQ(send_byte(b, 'x'), 1);
        auto const give_up =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool each_ran_one = true;
        while (!got && each_ran_one &&
               std::chrono::steady_clock::now() < give_up)
            each_ran_one = ioc.run_one() == 1;
        BOOST_TEST(each_ran_one);
        BOOST_TEST(got);

        quit = true;
        ioc.run();
    }

    /* A TCP read that follows a write on its socket is queued behind
       the completions the port holds, and its request is issued when
       the loop gets to it. Until then nothing of it is in the kernel,
       so a close, a cancel or a stop has to end it by other means.

       The read waits only behind a packet the port holds, and `body`
       puts one there for certain: a stop that is taken back at once
       leaves its packet behind. The port is looked at only while the
       kernel owes a completion, which the read of `idle` sees to.
       `actor` was queued before the read is, so it runs first and
       acts on it.

       The peer has sent a byte by then. A read already in the kernel
       would have taken it and reported it, so `canceled` also shows
       that the request had not been issued.
    */
    template<class Act>
    void lateReadEndsWith(Act act, std::stop_token token = {})
    {
        io_context ioc(iocp);
        auto ex       = ioc.get_executor();
        auto [a, b]   = test::must(test::make_socket_pair(ioc));
        auto [c, d]   = test::must(test::make_socket_pair(ioc));
        char buf[8]   = {};
        bool resumed  = false;
        std::size_t n = 99;
        std::error_code rec;
        auto idle = [&]() -> capy::task<> {
            char x[1];
            std::ignore =
                co_await d.read_some(capy::mutable_buffer(x, sizeof(x)));
        };
        auto body = [&]() -> capy::task<> {
            ioc.stop();
            ioc.restart();
            char const byte = 'x';
            auto [wec, wn] =
                co_await a.write_some(capy::const_buffer(&byte, 1));
            BOOST_TEST(!wec);
            BOOST_TEST_EQ(wn, 1u);
            auto [ec, got] =
                co_await a.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            rec     = ec;
            n       = got;
            resumed = true;
            // Ends `idle`, and the run with it.
            d.cancel();
        };
        auto actor = [&]() -> capy::task<> {
            BOOST_TEST_EQ(send_byte(b, 'y'), 1);
            act(a);
            co_return;
        };
        capy::run_async(ex)(idle());
        capy::run_async(ex, token)(body());
        capy::run_async(ex)(actor());
        ioc.run();
        BOOST_TEST(resumed);
        BOOST_TEST(rec == capy::cond::canceled);
        BOOST_TEST_EQ(n, 0u);
    }

    void testLateReadClosedBeforeItStarts()
    {
        lateReadEndsWith([](tcp_socket& a) { a.close(); });
    }

    void testLateReadCancelledBeforeItStarts()
    {
        lateReadEndsWith([](tcp_socket& a) { a.cancel(); });
    }

    void testLateReadStoppedBeforeItStarts()
    {
        std::stop_source ss;
        lateReadEndsWith(
            [&](tcp_socket&) { ss.request_stop(); }, ss.get_token());
    }

    void testLateReadReleasedBeforeItStarts()
    {
        SOCKET raw = INVALID_SOCKET;
        lateReadEndsWith([&](tcp_socket& a) {
            auto [ec, fd] = a.release();
            BOOST_TEST(!ec);
            raw = static_cast<SOCKET>(fd);
            // The byte is still there: no request of the library
            // took it. Waited for with a bound, so that a byte that
            // is gone fails the test instead of blocking it.
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(raw, &readable);
            timeval bound{10, 0};
            int const ready =
                ::select(0, &readable, nullptr, nullptr, &bound);
            BOOST_TEST_EQ(ready, 1);
            char byte = 0;
            if (ready == 1)
                BOOST_TEST_EQ(::recv(raw, &byte, 1, 0), 1);
            BOOST_TEST_EQ(byte, 'y');
        });
        BOOST_TEST(raw != INVALID_SOCKET);
        ::closesocket(raw);
    }

    // The context goes away with such a read queued: the read is
    // destroyed with the rest of the queue, and its coroutine with it.
    void testDestroyWithLateReadQueued()
    {
        struct count_on_destroy
        {
            int* n;
            ~count_on_destroy()
            {
                ++*n;
            }
        };
        int destroyed = 0;
        bool resumed  = false;
        {
            io_context ioc(iocp);
            auto ex     = ioc.get_executor();
            auto [a, b] = test::must(test::make_socket_pair(ioc));
            auto [c, d] = test::must(test::make_socket_pair(ioc));
            char buf[8] = {};
            auto idle = [&]() -> capy::task<> {
                count_on_destroy guard{&destroyed};
                char x[1];
                std::ignore =
                    co_await d.read_some(capy::mutable_buffer(x, sizeof(x)));
                resumed = true;
            };
            auto body = [&]() -> capy::task<> {
                count_on_destroy guard{&destroyed};
                // As in lateReadEndsWith.
                ioc.stop();
                ioc.restart();
                char const byte = 'x';
                std::ignore =
                    co_await a.write_some(capy::const_buffer(&byte, 1));
                std::ignore = co_await a.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                resumed = true;
            };
            capy::run_async(ex)(idle());
            capy::run_async(ex)(body());
            // One call each: `idle` up to its read, `body` up to the
            // read it queues behind the packet of the stop.
            BOOST_TEST_EQ(ioc.run_one(), 1u);
            BOOST_TEST_EQ(ioc.run_one(), 1u);
        }
        BOOST_TEST(!resumed);
        BOOST_TEST_EQ(destroyed, 2);
    }

    // Sockets this library opens or accepts are in skip-on-success
    // mode, and the mode outlives release(): the released socket must
    // come back in through assign().
    void testReleasedTcpSocketIsAdoptedAgain()
    {
        io_context ioc(iocp);
        auto [a, b] = test::must(test::make_socket_pair(ioc));
        BOOST_TEST(exchanges(ioc, a, b));

        auto [aec, afd] = a.release();
        BOOST_TEST(!aec);
        auto [bec, bfd] = b.release();
        BOOST_TEST(!bec);

        tcp_socket a2(ioc);
        tcp_socket b2(ioc);
        BOOST_TEST(!a2.assign(afd));
        BOOST_TEST(!b2.assign(bfd));
        BOOST_TEST(exchanges(ioc, a2, b2));
    }

    // A TCP socket this library opened is in skip-on-success mode for
    // good, and may be released to become a listener. The acceptor
    // adopts it, and accepts whether the connection was already
    // queued, which AcceptEx settles during the call with no packet,
    // or arrives later.
    void testReleasedTcpSocketIsAdoptedAsListener()
    {
        io_context ioc(iocp);
        tcp_socket opened(ioc);
        BOOST_TEST(!opened.open(family::v4));
        auto [rec, lfd] = opened.release();
        BOOST_TEST(!rec);
        SOCKET const ls = static_cast<SOCKET>(lfd);

        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        BOOST_TEST(
            ::bind(ls, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);
        BOOST_TEST(::listen(ls, SOMAXCONN) == 0);

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.assign(lfd));
        BOOST_TEST(acc.is_open());
        sa.sin_port = htons(acc.local_endpoint().port());
        BOOST_TEST(sa.sin_port != 0);

        for (bool queued_first : {true, false})
        {
            SOCKET raw = ::WSASocketW(
                AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                WSA_FLAG_OVERLAPPED);
            BOOST_TEST(raw != INVALID_SOCKET);
            // The listen backlog completes the handshake, so this
            // returns before anything accepts.
            auto connect_raw = [&] {
                BOOST_TEST(
                    ::connect(
                        raw, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) ==
                    0);
            };

            tcp_socket server(ioc);
            std::error_code aec = std::make_error_code(std::errc::io_error);
            auto accept_t       = [&]() -> capy::task<> {
                auto [e] = co_await acc.accept(server);
                aec      = e;
            };

            if (queued_first)
                connect_raw();
            capy::run_async(ioc.get_executor())(accept_t());
            if (!queued_first)
            {
                // Issue the AcceptEx with nothing to accept yet.
                std::ignore = ioc.poll();
                ioc.restart();
                connect_raw();
            }
            ioc.run();
            ioc.restart();
            BOOST_TEST(!aec);
            BOOST_TEST(server.is_open());

            tcp_socket client(ioc);
            BOOST_TEST(!client.assign(static_cast<native_handle_type>(raw)));
            BOOST_TEST(exchanges(ioc, client, server));
        }
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
        auto const invalid = detail::invalid_native_handle;

        test::temp_socket_dir tmp;
        local_stream_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(test::must(make_local_endpoint(tmp.path()))));
        BOOST_TEST(!acc.listen());

        auto [fd_ec, fd] = acc.release();
        BOOST_TEST(!fd_ec);
        BOOST_TEST(fd != invalid);
        ::closesocket(static_cast<SOCKET>(fd));

        BOOST_TEST(acc.native_handle() == invalid);
        BOOST_TEST(
            acc.set_option(socket_option::reuse_address(true)) ==
            std::errc::bad_file_descriptor);
        auto [acc_gec, acc_gopt] =
            acc.get_option<socket_option::reuse_address>();
        BOOST_TEST(acc_gec == std::errc::bad_file_descriptor);
        BOOST_TEST(!acc_gopt.value());
    }

    void testTruncateWithoutCreate()
    {
        temp_file tmp("corosio_iocp_", "some existing content");
        io_context ioc(iocp);
        random_access_file f(ioc);
        BOOST_TEST(
            !f.open(tmp.path, file_base::write_only | file_base::truncate));
        auto [size_ec, size_n] = f.size();
        BOOST_TEST(!size_ec);
        BOOST_TEST_EQ(size_n, 0u);
        f.close();
    }

    // close() used to ask the wait reactor to drop the object's wait op
    // even when none was ever parked, so objects destroyed before the
    // reactor thread existed left those asks queued; the first wait
    // started the thread, which must not touch the dead ops (ASan
    // catches it). An ask now needs both a running thread and a parked
    // op, so these objects ask nothing, and the scenario only shows
    // that the wait after them still completes.
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
        auto [s1, s2]      = test::must(test::make_socket_pair(ioc));
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
        auto [r1, r2] = test::must(test::make_socket_pair(ioc));
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
        BOOST_TEST(!lacc.bind(test::must(make_local_endpoint(ltmp.path()))));
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

        auto release_of = [](auto& obj) {
            auto [rel_ec, rel_h] = obj.release();
            BOOST_TEST(!rel_ec);
            return rel_h;
        };
        native_handle_type released[] = {release_of(r1),   release_of(acc),
                                         release_of(conn), release_of(u),
                                         release_of(l1),   release_of(lacc)};

        std::ignore = ioc.run_for(std::chrono::seconds(30));
        BOOST_TEST_EQ(done, 6);
        for (auto const& e : ec)
            BOOST_TEST(e == capy::cond::canceled);

        for (auto h : released)
            ::closesocket(static_cast<SOCKET>(h));
    }

    // Starts `op`, then closes `obj` while a second handle keeps its
    // socket open, and checks the op ends canceled.
    template<class Object, class Op>
    void
    closeCancelsWithSecondHandle(io_context& ioc, Object& obj, Op const& op)
    {
        SOCKET const dup = duplicate_socket(obj.native_handle());
        BOOST_TEST(dup != INVALID_SOCKET);

        std::error_code ec;
        bool done = false;
        capy::run_async(ioc.get_executor())(op(ec, done));
        std::ignore = ioc.poll();
        ioc.restart();
        BOOST_TEST(!done);

        obj.close();
        // Bounded only to fail rather than hang.
        std::ignore = ioc.run_for(std::chrono::seconds(5));
        ioc.restart();
        BOOST_TEST(done);
        BOOST_TEST(ec == capy::cond::canceled);

        // Closing the last handle aborts whatever close() left in the
        // kernel, so a failed check still ends the op while `ec` and
        // `done` are alive.
        ::closesocket(dup);
        if (!done)
        {
            std::ignore = ioc.run_for(std::chrono::seconds(5));
            ioc.restart();
        }
    }

    // closesocket() aborts pending I/O only when it closes the last
    // handle to the socket, so with an inherited or duplicated handle
    // open, close() must cancel the ops it leaves in the kernel itself.
    // Each op runs alone on its socket, so a cancel aimed at another op
    // cannot end it.
    void testCloseCancelsWithSecondHandle()
    {
        io_context ioc(iocp);

        // Without SO_LINGER: a zero-linger close resets the connection
        // even through a handle that is not the last, which would end
        // the op with no cancel at all.
        auto graceful_pair = [&] {
            return test::must(
                test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc));
        };

        // read_some: a WSARecv on a socket nothing is sent to.
        auto [r1, r2] = graceful_pair();
        char rbuf[8];
        closeCancelsWithSecondHandle(
            ioc, r1, [&](std::error_code& ec, bool& done) -> capy::task<> {
                auto [e, n] = co_await r1.read_some(
                    capy::mutable_buffer(rbuf, sizeof(rbuf)));
                std::ignore = n;
                ec          = e;
                done        = true;
            });

        // wait(read): the zero-byte WSARecv.
        auto [w1, w2] = graceful_pair();
        closeCancelsWithSecondHandle(
            ioc, w1, [&](std::error_code& ec, bool& done) -> capy::task<> {
                auto [e] = co_await w1.wait(wait_type::read);
                ec       = e;
                done     = true;
            });

        // write_some: with no send buffer, a WSASend larger than the
        // peer's receive window stays in flight while the peer never
        // reads.
        auto [s1, s2] = graceful_pair();
        test::must_ok(s1.set_option(socket_option::send_buffer_size(0)));
        std::vector<char> big(4 * 1024 * 1024);
        closeCancelsWithSecondHandle(
            ioc, s1, [&](std::error_code& ec, bool& done) -> capy::task<> {
                auto [e, n] = co_await s1.write_some(
                    capy::const_buffer(big.data(), big.size()));
                std::ignore = n;
                ec          = e;
                done        = true;
            });

        // connect: a listener in conditional-accept mode that never
        // accepts leaves the SYN unanswered, so the connect stays in
        // flight until it times out some 20 seconds later.
        tcp_acceptor held(ioc);
        BOOST_TEST(!held.open(family::v4));
        BOOL const on = TRUE;
        BOOST_TEST(
            ::setsockopt(
                static_cast<SOCKET>(held.native_handle()), SOL_SOCKET,
                SO_CONDITIONAL_ACCEPT, reinterpret_cast<char const*>(&on),
                sizeof(on)) == 0);
        BOOST_TEST(!held.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!held.listen());
        tcp_socket conn(ioc);
        BOOST_TEST(!conn.open(family::v4));
        closeCancelsWithSecondHandle(
            ioc, conn, [&](std::error_code& ec, bool& done) -> capy::task<> {
                auto [e] = co_await conn.connect(held.local_endpoint());
                ec       = e;
                done     = true;
            });

        // accept: an AcceptEx no client connects to.
        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open(family::v4));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        tcp_socket peer(ioc);
        closeCancelsWithSecondHandle(
            ioc, acc, [&](std::error_code& ec, bool& done) -> capy::task<> {
                auto [e] = co_await acc.accept(peer);
                ec       = e;
                done     = true;
            });
    }

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
            std::ignore =
                lacc.bind(test::must(make_local_endpoint(tmp2.path())));
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
            auto [a1, a2] = test::must(test::make_socket_pair(ioc));
            auto [b1, b2] = test::must(test::make_socket_pair(ioc));

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

    void run()
    {
        testStopCancelsLocalStreamOps();
        testStopCancelsAcceptorWaits();
        testAssignValidation();
        testAssignRejectsSkipOnSuccessSockets();
        testAssignAdoptsSkipOnSuccessTcpSocket();
        testReleasedTcpSocketIsAdoptedAsListener();
        testReleasedTcpSocketIsAdoptedAgain();
        testPostAfterTimerKeepsRunning();
        testRestartAfterStopKeepsRunning();
        testSecondLoopRunsWorkBehindBlockedHandler();
        testPostsAcrossRestartKeepTheirOrder();
        testPostsThroughThePortKeepTheirOrder();
        testOutsidePostsRunInOrder();
        testOutsidePostsWakeBlockedLoops();
        testContextDestroyedBehindOutsidePost();
        testRunOneReachesPortBehindQueuedWork();
        // Only a write that completes during the call is followed by
        // a read started late, and without skip-on-success mode none
        // does.
        if (detail::iocp_skip_on_success_enabled())
        {
            testLateReadClosedBeforeItStarts();
            testLateReadCancelledBeforeItStarts();
            testLateReadStoppedBeforeItStarts();
            testLateReadReleasedBeforeItStarts();
            testDestroyWithLateReadQueued();
        }
        testStopRacingWaitRegistration();
        testShutdownReceiveVariants();
        testZeroLengthUdpReceive();
        testResolverEmptyInputs();
        testReleasedAcceptorAccessors();
        testTruncateWithoutCreate();
        testStaleWaitCancelsFromDestroyedObjects();
        testReleaseWithKernelOpsInFlight();
        testCloseCancelsWithSecondHandle();
        testDestroyWithParkedSocketOps();
        testDestroyWithParkedLocalOps();
        testDestroyWithParkedTcpStreamOps();
    }
};

TEST_SUITE(iocp_paths_test, "boost.corosio.iocp_paths");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP
