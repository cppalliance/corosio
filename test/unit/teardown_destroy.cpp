//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/delay.hpp>
#include <boost/corosio/detail/thread_pool.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/random_access_file.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/signal_set.hpp>
#include <boost/corosio/stream_file.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_server.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/strand.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "context.hpp"
#include "pool_teardown.hpp"
#include "temp_path.hpp"
#include "must.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

// Tearing down a context destroys every frame suspended in one of its
// operations, and with it the chain awaiting that frame, exactly once.
template<auto Backend>
struct teardown_destroy_test
{
#if BOOST_COROSIO_HAS_URING
    static constexpr bool on_ring =
        std::is_same_v<std::remove_const_t<decltype(Backend)>, uring_t>;
#else
    static constexpr bool on_ring = false;
#endif

    // Frames from the recycling allocator are never really freed, which
    // hides a use-after-free from ASan; these go through operator new.
    static auto spawn(io_context& ioc)
    {
        return capy::run_async(
            ioc.get_executor(), std::allocator<std::byte>{});
    }

    struct destroy_count_t
    {
        int* n;
        explicit destroy_count_t(int& c) : n(&c) {}
        destroy_count_t(destroy_count_t&& o) noexcept
            : n(std::exchange(o.n, nullptr)) {}
        ~destroy_count_t()
        {
            if (n)
                ++*n;
        }
    };

    void testParkedReadDestroysChain()
    {
        int child_down = 0, parent_down = 0;
        {
            io_context ioc(Backend);
            auto [a, b] = test::must(test::make_socket_pair(ioc));
            auto child = [](tcp_socket& s, destroy_count_t) -> capy::task<> {
                char c;
                std::ignore = co_await s.read_some(capy::mutable_buffer(&c, 1));
            };
            auto parent = [&](tcp_socket& s, destroy_count_t) -> capy::task<> {
                co_await child(s, destroy_count_t(child_down));
            };
            spawn(ioc)(
                parent(a, destroy_count_t(parent_down)));
            std::ignore = ioc.poll();
            BOOST_TEST_EQ(child_down, 0);
            BOOST_TEST_EQ(parent_down, 0);
        }
        BOOST_TEST_EQ(child_down, 1);
        BOOST_TEST_EQ(parent_down, 1);
    }

    // The frame owns the socket whose impl stores the parked op, so
    // destroying the frame releases that impl while the op is still
    // inside its own teardown.
    void testParkedReadOwningSocketDestroysChain()
    {
        int down = 0;
        {
            io_context ioc(Backend);
            auto [a, b] = test::must(test::make_socket_pair(ioc));
            auto owner = [](tcp_socket s, destroy_count_t) -> capy::task<> {
                char c;
                std::ignore = co_await s.read_some(capy::mutable_buffer(&c, 1));
            };
            spawn(ioc)(
                owner(std::move(a), destroy_count_t(down)));
            std::ignore = ioc.poll();
            BOOST_TEST_EQ(down, 0);
        }
        BOOST_TEST_EQ(down, 1);
    }

    void testParkedAcceptDestroysChain()
    {
        int parent_down = 0;
        {
            io_context ioc(Backend);
            tcp_acceptor acc(ioc);
            BOOST_TEST(!acc.open());
            test::must_ok(acc.set_option(socket_option::reuse_address(true)));
            BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
            BOOST_TEST(!acc.listen());
            auto parent = [](tcp_acceptor& a, destroy_count_t) -> capy::task<> {
                std::ignore = co_await a.accept();
            };
            spawn(ioc)(
                parent(acc, destroy_count_t(parent_down)));
            std::ignore = ioc.poll();
            BOOST_TEST_EQ(parent_down, 0);
        }
        BOOST_TEST_EQ(parent_down, 1);
    }

    void testParkedSignalWaitDestroysChain()
    {
        int parent_down = 0;
        {
            io_context ioc(Backend);
            signal_set sigs(ioc, SIGINT);
            auto parent = [](signal_set& s, destroy_count_t) -> capy::task<> {
                std::ignore = co_await s.wait();
            };
            spawn(ioc)(
                parent(sigs, destroy_count_t(parent_down)));
            std::ignore = ioc.poll();
            BOOST_TEST_EQ(parent_down, 0);
        }
        BOOST_TEST_EQ(parent_down, 1);
    }

    // The frame owns the set whose impl stores the parked wait.
    void testParkedSignalWaitOwningSetDestroysChain()
    {
        int down = 0;
        {
            io_context ioc(Backend);
            auto owner = [](signal_set s, destroy_count_t) -> capy::task<> {
                std::ignore = co_await s.wait();
            };
            spawn(ioc)(
                owner(signal_set(ioc, SIGINT), destroy_count_t(down)));
            std::ignore = ioc.poll();
            BOOST_TEST_EQ(down, 0);
        }
        BOOST_TEST_EQ(down, 1);
    }

#if BOOST_COROSIO_POSIX
    // The read waits behind a parked worker, so at teardown it is still
    // queued on the blocking pool or its completion is still queued on
    // the scheduler; either way its frame is destroyed.
    void testQueuedFileReadDestroysChain()
    {
        int parent_down = 0;
        test::temp_file tmp("teardown_destroy_", "hello world");
        test::pool_blocker blocker;
        {
            io_context ioc(Backend);
            BOOST_TEST(test::park_pool_worker(ioc, blocker));
            random_access_file f(ioc);
            BOOST_TEST(!f.open(tmp.path, file_base::read_only));
            auto parent = [](random_access_file& rf,
                             destroy_count_t) -> capy::task<> {
                char c;
                std::ignore =
                    co_await rf.read_some_at(0, capy::mutable_buffer(&c, 1));
            };
            spawn(ioc)(
                parent(f, destroy_count_t(parent_down)));
            std::ignore = ioc.poll();
        }
        BOOST_TEST_EQ(parent_down, 1);
    }
#endif

    // Each operation still queued on the pool when it shuts down is
    // abandoned there, before the context goes, and its blocking call
    // never runs.
    template<class Start>
    void checkPoolShutdownDestroys(Start start)
    {
        int parent_down = 0;
        test::pool_blocker blocker;
        io_context ioc(Backend);
        BOOST_TEST(test::park_pool_worker(ioc, blocker));
        start(ioc, destroy_count_t(parent_down));
        std::ignore = ioc.poll();
        BOOST_TEST_EQ(parent_down, 0);
        test::shutdown_pool_behind(ioc, blocker);
        // The pool abandons the chain; its unwind runs on the context.
        ioc.restart();
        std::ignore = ioc.poll();
        BOOST_TEST_EQ(parent_down, 1);
    }

    void testPoolShutdownDestroysQueuedOps()
    {
        // IOCP reads files and resolves names without the pool; only
        // its reverse resolve uses it.
#if BOOST_COROSIO_POSIX
        test::temp_file tmp("teardown_destroy_", "hello world");

        // io_uring reads files through the ring, not the pool.
        if constexpr (!on_ring)
        {
            checkPoolShutdownDestroys([&](io_context& ioc, destroy_count_t d) {
                auto op = [](random_access_file f,
                             destroy_count_t) -> capy::task<> {
                    char c;
                    std::ignore =
                        co_await f.read_some_at(0, capy::mutable_buffer(&c, 1));
                };
                random_access_file f(ioc);
                BOOST_TEST(!f.open(tmp.path, file_base::read_only));
                spawn(ioc)(
                    op(std::move(f), std::move(d)));
            });
            checkPoolShutdownDestroys([&](io_context& ioc, destroy_count_t d) {
                auto op = [](stream_file f, destroy_count_t) -> capy::task<> {
                    char c;
                    std::ignore =
                        co_await f.read_some(capy::mutable_buffer(&c, 1));
                };
                stream_file f(ioc);
                BOOST_TEST(!f.open(tmp.path, file_base::read_only));
                spawn(ioc)(
                    op(std::move(f), std::move(d)));
            });
        }
        checkPoolShutdownDestroys([](io_context& ioc, destroy_count_t d) {
            auto op = [](resolver r, destroy_count_t) -> capy::task<> {
                std::ignore = co_await r.resolve(
                    "127.0.0.1", "80",
                    resolve_flags::numeric_host |
                        resolve_flags::numeric_service);
            };
            spawn(ioc)(
                op(resolver(ioc), std::move(d)));
        });
#endif
        checkPoolShutdownDestroys([](io_context& ioc, destroy_count_t d) {
            auto op = [](resolver r, destroy_count_t) -> capy::task<> {
                std::ignore = co_await r.resolve(
                    endpoint(ipv4_address::loopback(), 80),
                    reverse_flags::numeric_host |
                        reverse_flags::numeric_service);
            };
            spawn(ioc)(
                op(resolver(ioc), std::move(d)));
        });
    }

#if BOOST_COROSIO_POSIX
    // A blocking-pool operation is work on the context that owns it,
    // whichever context the awaiting coroutine runs on.
    void testPoolOpIsWorkOnItsOwnContext()
    {
        test::pool_blocker blocker;
        io_context a(Backend);
        io_context b(Backend);
        BOOST_TEST(test::park_pool_worker(a, blocker));
        resolver r(a);
        bool done = false;
        auto op = [](resolver& rs, bool& d) -> capy::task<> {
            std::ignore = co_await rs.resolve(
                "127.0.0.1", "80",
                resolve_flags::numeric_host |
                    resolve_flags::numeric_service);
            d = true;
        };
        spawn(b)(op(r, done));
        std::ignore = b.poll();
        std::ignore = a.poll();
        BOOST_TEST(!a.stopped());
        if (a.stopped())
            a.restart();
        blocker.release();
        std::ignore = a.run();
        std::ignore = b.run();
        BOOST_TEST(done);
    }
#endif

#if BOOST_COROSIO_POSIX
    // A session runs on another context, as the launcher allows, and
    // awaits a blocking-pool operation of the server's context.
    struct pool_session_worker : tcp_server::worker_base
    {
        tcp_socket sock_;
        io_context& session_ctx_;
        bool use_file_;
        random_access_file* file_;
        resolver* resolver_;
        int& down_;
        bool& launched_;

        pool_session_worker(
            io_context& server_ctx,
            io_context& session_ctx,
            bool use_file,
            random_access_file* file,
            resolver* r,
            int& down,
            bool& launched)
            : sock_(server_ctx)
            , session_ctx_(session_ctx)
            , use_file_(use_file)
            , file_(file)
            , resolver_(r)
            , down_(down)
            , launched_(launched)
        {
        }

        tcp_socket& socket() override
        {
            return sock_;
        }

        void run(tcp_server::launcher launch) override
        {
            launched_ = true;
            if (use_file_)
                launch(
                    session_ctx_.get_executor(),
                    [](random_access_file f, destroy_count_t) -> capy::task<> {
                        char c;
                        std::ignore = co_await f.read_some_at(
                            0, capy::mutable_buffer(&c, 1));
                    }(std::move(*file_), destroy_count_t(down_)));
            else
                launch(
                    session_ctx_.get_executor(),
                    [](resolver r, destroy_count_t) -> capy::task<> {
                        std::ignore = co_await r.resolve(
                            "127.0.0.1", "80",
                            resolve_flags::numeric_host |
                                resolve_flags::numeric_service);
                    }(std::move(*resolver_), destroy_count_t(down_)));
        }
    };

    struct pool_session_server : tcp_server
    {
        pool_session_server(
            io_context& server_ctx,
            io_context& session_ctx,
            bool use_file,
            random_access_file* file,
            resolver* r,
            int& down,
            bool& launched)
            : tcp_server(server_ctx, server_ctx.get_executor())
        {
            std::vector<std::unique_ptr<tcp_server::worker_base>> v;
            v.push_back(std::make_unique<pool_session_worker>(
                server_ctx, session_ctx, use_file, file, r, down, launched));
            set_workers(std::move(v));
        }
    };

    // Neither context may be destroyed while such a session waits: the
    // unwind of a context's teardown runs on the session's context and
    // would outlive the server's. Stopping the server cancels the
    // operation, and running both contexts ends the session first.
    void checkCrossContextSessionDrains(bool use_file)
    {
        int down      = 0;
        bool launched = false;
        test::temp_file tmp("teardown_destroy_", "hello world");
        test::pool_blocker blocker;
        auto a = std::make_unique<io_context>(Backend);
        auto b = std::make_unique<io_context>(Backend);
        BOOST_TEST(test::park_pool_worker(*a, blocker));
        std::optional<random_access_file> f(std::in_place, *a);
        BOOST_TEST(!f->open(tmp.path, file_base::read_only));
        std::optional<resolver> r(std::in_place, *a);
        {
            pool_session_server srv(*a, *b, use_file, &*f, &*r, down, launched);
            BOOST_TEST(!srv.bind(endpoint(ipv4_address::loopback(), 0)));
            auto ep = srv.local_endpoint();
            srv.start();
            tcp_socket c(*a);
            spawn(*a)([](tcp_socket& s, endpoint e) -> capy::task<> {
                std::ignore = co_await s.connect(e);
            }(c, ep));
            while (!launched && a->run_one_for(std::chrono::seconds(5)) != 0)
            {
            }
            BOOST_TEST(launched);
            std::ignore = b->poll();
            std::ignore = a->poll();
            BOOST_TEST_EQ(down, 0);

            srv.stop();
            blocker.release();
            // Bounds a hang, not a timing assertion.
            for (int i = 0; down == 0 && i < 500; ++i)
            {
                a->restart();
                b->restart();
                std::ignore = a->run_one_for(std::chrono::milliseconds(10));
                std::ignore = b->poll();
            }
            BOOST_TEST_EQ(down, 1);
            a->restart();
            std::ignore = a->poll();
        }
        f.reset();
        r.reset();
        a.reset();
        b.reset();
    }

    void testCrossContextSessionDrains()
    {
        // io_uring reads files through the ring, not the pool.
        if constexpr (!on_ring)
            checkCrossContextSessionDrains(true);
        checkCrossContextSessionDrains(false);
    }
#endif

    // Records, when the context destroys its services, how many frames
    // were already destroyed. Made last, it is destroyed first.
    struct teardown_witness : capy::execution_context::service
    {
        int const* down;
        int* seen;

        teardown_witness(capy::execution_context&, int const& d, int& s)
            : down(&d)
            , seen(&s)
        {
        }

        ~teardown_witness()
        {
            *seen = *down;
        }

        void shutdown() override {}
    };

    // Shutting a context down destroys every frame suspended in it before
    // any service is destroyed. The frames that own their I/O object, the
    // timers and the pool read are still held by a service when it shuts
    // down; those that borrow one are cancelled when it closes, before
    // the context goes.
    void testShutdownDestroysFramesBeforeServices()
    {
        int down     = 0;
        int seen     = -1;
        int expected = 0;
        auto counted = [&] {
            ++expected;
            return destroy_count_t(down);
        };
#if BOOST_COROSIO_POSIX
        test::temp_file tmp("teardown_destroy_", "hello world");
        test::pool_blocker blocker;
#endif
        {
            io_context ioc(Backend);
            auto [a, b] = test::must(test::make_socket_pair(ioc));
            auto [c, d] = test::must(test::make_socket_pair(ioc));
            tcp_acceptor acc(ioc);
            BOOST_TEST(!acc.open());
            test::must_ok(acc.set_option(socket_option::reuse_address(true)));
            BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
            BOOST_TEST(!acc.listen());
            tcp_acceptor owned(ioc);
            BOOST_TEST(!owned.open());
            BOOST_TEST(
                !owned.bind(endpoint(ipv4_address::loopback(), 0)));
            BOOST_TEST(!owned.listen());
            signal_set sigs(ioc, SIGINT);
            capy::strand st(ioc.get_executor());

            auto on_delay = [](destroy_count_t) -> capy::task<> {
                std::ignore = co_await delay(std::chrono::hours(1));
            };
            auto on_read = [](tcp_socket& s, destroy_count_t) -> capy::task<> {
                char ch;
                std::ignore =
                    co_await s.read_some(capy::mutable_buffer(&ch, 1));
            };
            auto owns_socket = [](tcp_socket s,
                                  destroy_count_t) -> capy::task<> {
                char ch;
                std::ignore =
                    co_await s.read_some(capy::mutable_buffer(&ch, 1));
            };
            auto on_accept = [](tcp_acceptor& ac,
                                destroy_count_t) -> capy::task<> {
                std::ignore = co_await ac.accept();
            };
            auto owns_acceptor = [](tcp_acceptor ac,
                                    destroy_count_t) -> capy::task<> {
                std::ignore = co_await ac.accept();
            };
            auto on_signal = [](signal_set& ss,
                                destroy_count_t) -> capy::task<> {
                std::ignore = co_await ss.wait();
            };
            auto owns_set = [](signal_set ss, destroy_count_t) -> capy::task<> {
                std::ignore = co_await ss.wait();
            };

            spawn(ioc)(on_delay(counted()));
            spawn(ioc)(on_read(a, counted()));
            spawn(ioc)(owns_socket(std::move(c), counted()));
            spawn(ioc)(on_accept(acc, counted()));
            spawn(ioc)(
                owns_acceptor(std::move(owned), counted()));
            spawn(ioc)(on_signal(sigs, counted()));
            spawn(ioc)(
                owns_set(signal_set(ioc, SIGINT), counted()));
            capy::run_async(st, std::allocator<std::byte>{})(
                on_delay(counted()));
#if BOOST_COROSIO_POSIX
            // io_uring reads files through the ring, so a read there can
            // complete; elsewhere it waits behind the parked pool worker,
            // which is held until the pool's own shutdown disposes of it.
            std::optional<random_access_file> f;
            if constexpr (!on_ring)
            {
                BOOST_TEST(test::park_pool_worker(ioc, blocker, true));
                f.emplace(ioc);
                BOOST_TEST(!f->open(tmp.path, file_base::read_only));
                auto on_file = [](random_access_file& rf,
                                  destroy_count_t) -> capy::task<> {
                    char ch;
                    std::ignore = co_await rf.read_some_at(
                        0, capy::mutable_buffer(&ch, 1));
                };
                spawn(ioc)(on_file(*f, counted()));
            }
#endif
            std::ignore = ioc.poll();
            BOOST_TEST_EQ(down, 0);
            ioc.make_service<teardown_witness>(down, seen);
        }
        BOOST_TEST_EQ(seen, expected);
        BOOST_TEST_EQ(down, expected);
    }

    // Posts from a service's destructor, after the teardown drain.
    struct late_poster : capy::execution_context::service
    {
        io_context::executor_type ex;
        capy::continuation* c;

        late_poster(
            capy::execution_context&,
            io_context::executor_type e,
            capy::continuation& cc)
            : ex(e)
            , c(&cc)
        {
        }

        ~late_poster()
        {
            ex.post(*c);
        }

        void shutdown() override {}
    };

    // A service should not post from its destructor, but work one posts
    // anyway is destroyed with the context, not left queued.
    void testPostAfterDrainDestroyed()
    {
        int down = 0;
        auto probe = [](destroy_count_t) -> capy::task<> { co_return; };
        auto t = probe(destroy_count_t(down));
        capy::continuation c{t.handle()};
        t.release();
        {
            io_context ioc(Backend);
            ioc.make_service<late_poster>(ioc.get_executor(), c);
        }
        BOOST_TEST_EQ(down, 1);
    }

    void run()
    {
        testParkedReadDestroysChain();
        testParkedReadOwningSocketDestroysChain();
        testParkedAcceptDestroysChain();
        testParkedSignalWaitDestroysChain();
        testParkedSignalWaitOwningSetDestroysChain();
#if BOOST_COROSIO_POSIX
        testQueuedFileReadDestroysChain();
#endif
        testPoolShutdownDestroysQueuedOps();
#if BOOST_COROSIO_POSIX
        testPoolOpIsWorkOnItsOwnContext();
        testCrossContextSessionDrains();
#endif
        testShutdownDestroysFramesBeforeServices();
        testPostAfterDrainDestroyed();
    }
};

COROSIO_BACKEND_TESTS(teardown_destroy_test, "boost.corosio.teardown_destroy")

} // namespace boost::corosio
