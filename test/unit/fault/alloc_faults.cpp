//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Allocation-failure recovery arms. Each scenario runs once per
// allocation ordinal in a forked child, so every `new` in the window
// takes the fault on some pass; an ordinal that lands on an unguarded
// site may abort that child, which the sweep tolerates — the guarded
// arms record their coverage on the passes that reach them, and the
// parent asserts only on the unarmed control run.

#include <boost/corosio/detail/platform.hpp>

#include <boost/corosio/delay.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/stream_file.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_server.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/udp.hpp>
#include <boost/corosio/udp_socket.hpp>

#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <new>
#include <string>
#include <system_error>
#include <vector>

#include "fault.hpp"
#include "fault_test_utils.hpp"

#include <boost/corosio/backend.hpp>

#include "test_suite.hpp"

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace boost::corosio {

namespace {

using test::fault::fault_scope;
using test::fault::sys;

#if !defined(_WIN32)

// Assert the unarmed control run, then run `body` once per allocation
// ordinal in [1, limit], each pass in its own forked child with that
// ordinal armed inside `body`'s armed window. A child that aborts on
// an unguarded site is tolerated; coverage from every child is flushed
// before it exits. `body` receives the ordinal to arm, 0 for the
// unarmed control.
template<class F>
void
alloc_sweep(int limit, F body)
{
    BOOST_TEST(body(0u));
    for (int nth = 1; nth <= limit; ++nth)
    {
        pid_t pid = ::fork();
        BOOST_TEST(pid >= 0);
        if (pid < 0)
            return;
        if (pid == 0)
        {
            bool const ok = body(static_cast<unsigned>(nth));
            test::fault::flush_coverage_counters();
            std::_Exit(ok ? 0 : 1);
        }
        int status = 0;
        ::waitpid(pid, &status, 0);
    }
}

// Arm `cpp_new` for the enclosing scope when `nth` is nonzero; inert
// for the control run.
struct maybe_arm
{
    std::optional<fault_scope> scope;

    explicit maybe_arm(unsigned nth)
    {
        if (nth != 0)
            scope.emplace(sys::cpp_new, 0, nth);
    }
};

#endif // !_WIN32

} // namespace

struct alloc_fault_test
{
    void testArmFailsAllocation()
    {
        // Through volatile function pointers: an optimizer may elide a
        // plain unused `new` outright (allocation elision), leaving the
        // arm live to detonate on some later allocation.
        void* (*volatile op_new)(std::size_t) =
            static_cast<void* (*)(std::size_t)>(&::operator new);
        void* (*volatile op_new_nt)(std::size_t, std::nothrow_t const&) =
            static_cast<void* (*)(std::size_t, std::nothrow_t const&)>(
                &::operator new);

        // Valgrind (and some sanitizers) replace operator new with their
        // own, which shadows this harness's replacement, so the arm can
        // never fire. Probe once with the armed nothrow form: a non-null
        // result means the replacement is bypassed, and the arm-firing
        // assertions below would spuriously fail — skip them.
        {
            fault_scope probe(sys::cpp_new, 0);
            if (void* p = op_new_nt(1, std::nothrow))
            {
                ::operator delete(p);
                return;
            }
        }

        {
            fault_scope f(sys::cpp_new, 0);
            BOOST_TEST_THROWS(std::ignore = op_new(1), std::bad_alloc);
            BOOST_TEST(f.fired());
        }
        {
            fault_scope g(sys::cpp_new, 0);
            void* p = op_new_nt(1, std::nothrow);
            BOOST_TEST(p == nullptr);
            BOOST_TEST(g.fired());
            if (p != nullptr)
                ::operator delete(p);
        }

        // Disarmed again: allocation works and the scope stays quiet.
        std::unique_ptr<char> q(new char('x'));
        BOOST_TEST(*q == 'x');
    }

#if !defined(_WIN32)

    // A capped-slice clock delay re-publishes through rearm_wait on
    // every wake; a churn task keeps the timer heap growing so some
    // rearm lands on a capacity boundary and takes the armed failure,
    // which must finish that delay with an error instead of stranding
    // the frame.
    void testTimerRearmRecovery()
    {
        struct capped_traits
        {
            static std::chrono::system_clock::duration
            to_wait_duration(std::chrono::system_clock::duration d)
            {
                return (std::min)(d,
                                  std::chrono::system_clock::duration(
                                      std::chrono::milliseconds(2)));
            }
        };

        alloc_sweep(32, [](unsigned nth) {
            io_context ioc;
            auto ex  = ioc.get_executor();
            int done = 0;

            auto repeater = [&]() -> capy::task<> {
                std::ignore = co_await corosio::delay<capped_traits>(
                    std::chrono::system_clock::now() +
                    std::chrono::milliseconds(40));
                ++done;
            };
            auto churn = [&]() -> capy::task<> {
                for (int i = 0; i < 16; ++i)
                    std::ignore =
                        co_await corosio::delay(std::chrono::milliseconds(1));
            };
            for (int i = 0; i < 3; ++i)
                capy::run_async(ex)(repeater());
            for (int i = 0; i < 6; ++i)
                capy::run_async(ex)(churn());

            maybe_arm arm(nth);
            ioc.run();
            return done == 3;
        });
    }

    // select's descriptor registry: a failed growth reports ENOMEM to
    // the registering operation instead of corrupting the registry.
    void testSelectRegisterRecovery()
    {
#if BOOST_COROSIO_HAS_SELECT
        alloc_sweep(8, [](unsigned nth) {
            io_context ioc(select);
            auto ex = ioc.get_executor();
            // Warm the context and registry machinery before arming.
            auto [w1, w2] =
                test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

            // Registration happens inside open(): the registry's node
            // allocation is one of the few in this window, and its
            // failure must surface as ENOMEM from open() with the
            // registry intact.
            maybe_arm arm(nth);
            udp_socket u(ioc);
            auto oec = u.open(udp::v4());
            if (oec)
                return oec == std::errc::not_enough_memory;

            char buf[4];
            bool completed = false;
            auto reader    = [&]() -> capy::task<> {
                auto [ec, n] =
                    co_await u.recv(capy::mutable_buffer(buf, sizeof(buf)));
                std::ignore = ec;
                std::ignore = n;
                completed   = true;
            };
            auto canceller = [&]() -> capy::task<> {
                u.cancel();
                co_return;
            };
            capy::run_async(ex)(reader());
            capy::run_async(ex)(canceller());
            ioc.run();
            return completed;
        });
#endif
    }

    // thread_pool spawn: an allocation failure while starting the
    // first worker must surface as a refusal from post(), which the
    // file service reports through the operation.
    void testPoolSpawnRecovery()
    {
        auto const path = test::fault::temp_path("alloc_pool");
        {
            std::ofstream(path) << "payload";
        }
        alloc_sweep(12, [&path](unsigned nth) {
            io_context ioc;
            stream_file f(ioc);
            if (f.open(path, file_base::read_only))
                return false;
            char buf[8];
            bool completed = false;
            auto reader    = [&]() -> capy::task<> {
                auto [ec, n] = co_await f.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                std::ignore = ec;
                std::ignore = n;
                completed   = true;
            };

            // The first post spawns the pool worker inside the armed
            // window; a refused spawn must complete the read with the
            // refusal, which still counts as completion.
            maybe_arm arm(nth);
            capy::run_async(ioc.get_executor())(reader());
            ioc.run();
            return completed;
        });
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    // tcp_server dispatch: a coroutine frame that fails to allocate
    // must hand the worker back to the pool and keep serving.
    void testServerLaunchRecovery()
    {
        alloc_sweep(24, [](unsigned nth) {
            io_context ioc;

            class echo_worker : public tcp_server::worker_base
            {
                io_context& ctx_;
                corosio::tcp_socket sock_;

            public:
                std::atomic<int>* count = nullptr;

                echo_worker(io_context& ctx, std::atomic<int>* c)
                    : ctx_(ctx)
                    , sock_(ctx)
                    , count(c)
                {
                }

                corosio::tcp_socket& socket() override
                {
                    return sock_;
                }

                void run(tcp_server::launcher launch) override
                {
                    count->fetch_add(1);
                    launch(
                        ctx_.get_executor(),
                        [](corosio::tcp_socket* s) -> capy::task<> {
                            s->close();
                            co_return;
                        }(&sock_));
                }
            };

            std::atomic<int> served{0};

            class one_server : public tcp_server
            {
            public:
                one_server(io_context& ctx, std::atomic<int>* c)
                    : tcp_server(ctx, ctx.get_executor())
                {
                    std::vector<std::unique_ptr<tcp_server::worker_base>> v;
                    v.push_back(std::make_unique<echo_worker>(ctx, c));
                    set_workers(std::move(v));
                }
            };

            one_server srv(ioc, &served);
            if (srv.bind(endpoint(ipv4_address::loopback(), 0)))
                return false;
            auto port = srv.local_endpoint().port();
            srv.start();

            auto driver = [](io_context* ctx, std::uint16_t p, one_server* s,
                             unsigned nth) -> capy::task<> {
                {
                    tcp_socket c(*ctx);
                    std::ignore                = c.open();
                    [[maybe_unused]] auto [ec] = co_await c.connect(
                        endpoint(ipv4_address::loopback(), p));
                    c.close();
                    std::ignore =
                        co_await corosio::delay(std::chrono::milliseconds(10));
                }
                {
                    // The dispatch of this connection allocates the
                    // session frame inside the armed window.
                    maybe_arm arm(nth);
                    tcp_socket c(*ctx);
                    std::ignore                = c.open();
                    [[maybe_unused]] auto [ec] = co_await c.connect(
                        endpoint(ipv4_address::loopback(), p));
                    std::ignore =
                        co_await corosio::delay(std::chrono::milliseconds(10));
                    c.close();
                }
                s->stop();
            }(&ioc, port, &srv, nth);
            capy::run_async(ioc.get_executor())(std::move(driver));
            ioc.run();
            srv.join();
            return served.load() >= 1;
        });
    }

#endif // !_WIN32

    void run()
    {
        testArmFailsAllocation();
#if !defined(_WIN32)
        testTimerRearmRecovery();
        testSelectRegisterRecovery();
        testPoolSpawnRecovery();
        testServerLaunchRecovery();
#endif
    }
};

TEST_SUITE(alloc_fault_test, "boost.corosio.fault.alloc");

} // namespace boost::corosio
