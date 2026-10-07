//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reactor-scheduler paths that need batched or contended dispatch: a
// descriptor event completing several parked ops at once, and a
// cond-parked follower woken by work posted from a foreign thread.
// Durations only bound blocking; nothing asserts elapsed time.

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL || BOOST_COROSIO_HAS_KQUEUE || \
    BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/wait_type.hpp>

#include <boost/corosio/test/socket_pair.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/thread_pool.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <stop_token>
#include <system_error>
#include <thread>
#include <tuple>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct mt_reactor_test
{
    void testEventCompletesBatchedOps()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        // A read and a readiness wait park on the same descriptor, so
        // one readable event dispatches both in a single batch. Two
        // bytes arrive and the read takes one, so readiness survives
        // the read and the wait can complete.
        char buf[1];
        std::error_code rec, wec;
        int done    = 0;
        auto reader = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            std::ignore = n;
            rec         = ec;
            ++done;
        };
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::read);
            wec       = ec;
            ++done;
        };
        auto trip = [&]() -> capy::task<> {
            char cc[2]  = {'z', 'z'};
            std::ignore = ::send(
                static_cast<int>(s2.native_handle()), cc, 2, MSG_NOSIGNAL);
            co_return;
        };
        capy::run_async(ex)(reader());
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(trip());
        ioc.run();

        BOOST_TEST_EQ(done, 2);
        BOOST_TEST(!rec);
        BOOST_TEST(!wec);
    }

    void testForeignPostWakesParkedFollower()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        // The parked read keeps outstanding work alive so both runner
        // threads stay inside the scheduler: one leads in the reactor,
        // the other parks in the signal wait. Each foreign post then
        // has a parked follower to wake.
        char buf[4];
        bool resumed = false;
        auto reader  = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            std::ignore = ec;
            std::ignore = n;
            resumed     = true;
        };
        capy::run_async(ex)(reader());

        std::atomic<int> entered{0};
        std::atomic<int> nops{0};
        auto slice = [&] {
            entered.fetch_add(1);
            for (int i = 0; i < 100 && nops.load() < 20; ++i)
                std::ignore = ioc.run_one_for(std::chrono::milliseconds(10));
        };
        std::thread ra(slice), rb(slice);

        // Yield while spinning: under Valgrind's serialized scheduler
        // a tight spin starves the freshly created runner threads of
        // their first slice, and the wait never completes.
        while (entered.load() < 2)
            std::this_thread::yield();
        for (int i = 0; i < 20; ++i)
        {
            // The counter travels as a parameter: a loop-scoped
            // closure would die before the runner threads execute the
            // frame that references it.
            capy::run_async(ex)([](std::atomic<int>* n) -> capy::task<> {
                n->fetch_add(1);
                co_return;
            }(&nops));
        }
        ra.join();
        rb.join();
        BOOST_TEST_EQ(nops.load(), 20);

        s1.cancel();
        ioc.restart();
        ioc.run();
        BOOST_TEST(resumed);
    }

    // An acceptor wait started off the reactor thread while that thread
    // blocks in the reactor. Select watches an fd only while an op is
    // parked on it, so the park has to wake the blocked reactor.
    void testAcceptorWaitWakesBlockedReactor()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        auto const port = acc.local_endpoint().port();

        // The parked read keeps run() inside the reactor.
        char buf[1];
        auto reader = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            std::ignore = ec;
            std::ignore = n;
        };
        capy::run_async(ex)(reader());
        std::thread runner([&] { ioc.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::atomic<bool> done{false};
        std::error_code wec;
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.wait(wait_type::read);
            wec       = ec;
            done.store(true);
        };
        // Runs the waiter on this thread up to the parked wait.
        capy::io_env env{ex, std::stop_token{}, nullptr};
        std::optional<capy::task<>> parked;
        parked.emplace(waiter());
        parked->await_suspend(std::noop_coroutine(), &env).resume();

        int client = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(client >= 0);
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_port        = htons(port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        BOOST_TEST_EQ(
            ::connect(client, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)),
            0);

        auto const deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        bool const woke = done.load();

        ioc.stop();
        runner.join();
        ::close(client);
        BOOST_TEST(woke);
        BOOST_TEST(!wec);

        // Drain the parked read, and the wait if it never woke.
        acc.cancel();
        s1.cancel();
        ioc.restart();
        ioc.run();
    }

    // A reactor pass can hold an event for a descriptor that another run
    // thread closes before the pass queues it. The closed socket's impl
    // must outlive that pass, not be recycled under it. Each iteration
    // makes a descriptor readable and destroys its socket while a second
    // run thread polls.
    void testCloseRacesReactorPass()
    {
        int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(lfd >= 0);
        int one = 1;
        ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len        = sizeof(addr);
        BOOST_TEST(
            ::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
            0);
        BOOST_TEST(
            ::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        BOOST_TEST(::listen(lfd, 64) == 0);

        io_context ioc(Backend, 2);
        int completed = 0;
        capy::run_async(ioc.get_executor())(
            [](io_context& c, int listener, sockaddr_in a,
               int& done) -> capy::task<> {
                for (int i = 0; i < 2000; ++i)
                {
                    int peer = ::socket(AF_INET, SOCK_STREAM, 0);
                    if (::connect(
                            peer, reinterpret_cast<sockaddr*>(&a),
                            sizeof(a)) != 0)
                    {
                        ::close(peer);
                        co_return;
                    }
                    int fd = ::accept(listener, nullptr, nullptr);
                    {
                        tcp_socket s(c);
                        std::ignore  = s.assign(fd);
                        char const b = 'x';
                        std::ignore  = ::send(peer, &b, 1, 0);
                    }
                    ::close(peer);
                    ++done;
                }
            }(ioc, lfd, addr, completed));

        std::thread second([&] { ioc.run(); });
        ioc.run();
        second.join();
        ::close(lfd);
        BOOST_TEST_EQ(completed, 2000);
    }

    // An op started from a thread that is not running the context posts
    // its completion through the scheduler lock. It must not do so while
    // holding the descriptor lock, which the reactor takes after the
    // scheduler lock when settling a descriptor closed during its pass.
    // A stop racing the start makes it complete without parking, taking
    // that posting path while the reactor settles the previous
    // incarnation's close.
    void testForeignStartRacesSettle()
    {
        int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(lfd >= 0);
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len        = sizeof(addr);
        BOOST_TEST(
            ::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
            0);
        BOOST_TEST(
            ::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        BOOST_TEST(::listen(lfd, 64) == 0);
        auto make_pair = [&](int& mine, int& peer) {
            peer = ::socket(AF_INET, SOCK_STREAM, 0);
            std::ignore = ::connect(
                peer, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            mine = ::accept(lfd, nullptr, nullptr);
        };

        io_context ioc(Backend, 2);
        capy::thread_pool pool(1);

        // A parked wait keeps run() polling until the end.
        int idle_fd  = -1;
        int idle_peer = -1;
        make_pair(idle_fd, idle_peer);
        tcp_socket idle(ioc);
        BOOST_TEST(!idle.assign(idle_fd));
        capy::run_async(ioc.get_executor())(
            [](tcp_socket& i) -> capy::task<> {
                std::ignore = co_await i.wait(wait_type::read);
            }(idle));
        std::thread runner([&] { ioc.run(); });

        tcp_socket s(ioc);
        int peer = -1;
        std::atomic<int> finished{0};
        for (int i = 0; i < 500; ++i)
        {
            if (peer >= 0)
                ::close(peer);
            s.close();
            int fd = -1;
            make_pair(fd, peer);
            BOOST_TEST(!s.assign(fd));
            std::stop_source stop;
            capy::run_async(pool.get_executor(), stop.get_token())(
                [](tcp_socket& x, std::atomic<int>& n) -> capy::task<> {
                    std::ignore = co_await x.wait(wait_type::read);
                    ++n;
                }(s, finished));
            stop.request_stop();
            while (finished.load() == i)
                std::this_thread::yield();
        }

        char const b = 'x';
        std::ignore  = ::send(idle_peer, &b, 1, 0);
        runner.join();
        pool.join();
        ::close(peer);
        ::close(idle_peer);
        ::close(lfd);
        BOOST_TEST_EQ(finished.load(), 500);
    }

    // An invocation popped from the queue clears the descriptor's
    // queued flag inside its critical section and keeps using the state
    // until it unlocks. Retiring the descriptor meanwhile must not take
    // that cleared flag to mean nothing still runs on it.
    void testRetireWaitsForRunningInvocation()
    {
        struct watched_impl : io_object::implementation
        {
            std::atomic<bool>* inside;
            std::atomic<bool>* retired;
            std::atomic<bool>* retired_inside;

            void retire() noexcept override
            {
                if (inside->load())
                    retired_inside->store(true);
                retired->store(true);
            }
        };

        // Stays inside the invocation's critical section until the
        // descriptor is retired, or long enough to show it was not.
        struct holding_op final : detail::reactor_op_base
        {
            std::atomic<bool>* inside;
            std::atomic<bool>* retired;

            void perform_io() noexcept override
            {
                inside->store(true);
                // Bounds the wait, not a timing assertion.
                auto const until = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(200);
                while (!retired->load() &&
                       std::chrono::steady_clock::now() < until)
                    std::this_thread::yield();
                inside->store(false);
                errn = EAGAIN;
            }
            void operator()() override {}
        };

        io_context ioc(Backend);
        auto& sched =
            static_cast<detail::reactor_scheduler&>(detail::get_scheduler(ioc));

        std::atomic<bool> inside{false};
        std::atomic<bool> retired{false};
        std::atomic<bool> retired_inside{false};
        watched_impl impl;
        impl.inside         = &inside;
        impl.retired        = &retired;
        impl.retired_inside = &retired_inside;
        impl.refs_          = 0;
        detail::object_ref keep(&impl);

        detail::reactor_descriptor_state desc;
        holding_op op;
        op.inside      = &inside;
        op.retired     = &retired;
        desc.scheduler_ = &sched;
        desc.read_op    = &op;
        desc.ready_events_.store(detail::reactor_event_read);
        desc.is_enqueued_.store(true);

        std::thread popped([&] { desc.invoke_deferred_io(); });
        while (!inside.load())
            std::this_thread::yield();
        sched.retire_descriptor(desc, std::move(keep));
        popped.join();

        BOOST_TEST(retired.load());
        BOOST_TEST(!retired_inside.load());
        desc.read_op = nullptr;
    }

    void run()
    {
        testRetireWaitsForRunningInvocation();
        testEventCompletesBatchedOps();
        testForeignPostWakesParkedFollower();
        testAcceptorWaitWakesBlockedReactor();
        testCloseRacesReactorPass();
        testForeignStartRacesSettle();
    }
};

COROSIO_REACTOR_BACKEND_TESTS(mt_reactor_test, "boost.corosio.mt_reactor")

} // namespace boost::corosio

#endif
