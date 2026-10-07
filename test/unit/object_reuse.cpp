//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Keepalive-extension regression for the object_pool conversion: dropping
// a socket while an op is still parked must not use-after-free the
// impl, and a socket constructed afterward on the same context must
// get a fully working impl, whether the pool recycled the dropped one
// or allocated a new one -- the caller cannot tell which, and must not
// need to.

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/signal_set.hpp>
#include <boost/corosio/stream_file.hpp>
#include <boost/corosio/delay.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <stop_token>
#include <system_error>

#include "context.hpp"
#include "temp_path.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct object_reuse_test
{
    // Open a connected pair, park a read on one side with no writer
    // on the other, then drop that socket while the read is still
    // parked. close()/destroy() cancel the read and release the
    // service's own reference; the read's own reference (its object_ref
    // keepalive) keeps the impl alive until the cancellation actually
    // drains, at which point the impl is recycled -- not freed. A
    // sanitizer build is what actually validates no UAF happened; this
    // test's own assertion is the behavioral half: the context keeps
    // working afterward.
    void testAbandonedReadThenReuse()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        // Declared at function scope, not inside the block below: if
        // the abandoned read's cancellation is ever actually delivered
        // (reactor backends always drain it; io_uring may instead
        // abandon it in the kernel ring at shutdown), the coroutine
        // resumes into this lambda and writes `resumed` — which must
        // still be live stack memory whenever that happens, including
        // after the block below has already exited.
        char buf[16];
        bool resumed = false;

        {
            auto [s1, s2] = test::make_socket_pair(ioc);
            std::ignore   = s2; // keeps the peer fd open; never written to

            capy::run_async(ex)(
                [](tcp_socket& s, char* b, std::size_t n,
                   bool& done) -> capy::task<> {
                    std::ignore =
                        co_await s.read_some(capy::mutable_buffer(b, n));
                    done = true;
                }(s1, buf, sizeof(buf), resumed));

            // Let the read actually register as parked before s1 (and
            // its parked read) go out of scope at the end of this block.
            std::ignore = ioc.run_one();
        }

        // Drain whatever the destructor's cancellation produced. This
        // must not crash, hang, or corrupt the pool regardless of
        // whether the cancellation's completion ever arrives (io_uring
        // may abandon it in the kernel ring at io_context shutdown;
        // the reactor backends always drain it here).
        ioc.run();
        ioc.restart();

        // A fresh socket on the same context must get a working impl.
        tcp_socket probe(ioc);
        BOOST_TEST(!probe.open());
        BOOST_TEST(probe.is_open());

        auto [p1, p2] = test::make_socket_pair(ioc);
        char const out = 'x';
        char in         = 0;
        bool wrote = false, read_ok = false;
        capy::run_async(ex)(
            [](tcp_socket& s, char const* b, bool& done) -> capy::task<> {
                auto [ec, n] = co_await s.write_some(capy::const_buffer(b, 1));
                done         = !ec && n == 1;
            }(p1, &out, wrote));
        capy::run_async(ex)(
            [](tcp_socket& s, char* b, bool& done) -> capy::task<> {
                auto [ec, n] = co_await s.read_some(capy::mutable_buffer(b, 1));
                done         = !ec && n == 1;
            }(p2, &in, read_ok));
        ioc.restart();
        ioc.run();

        BOOST_TEST(wrote);
        BOOST_TEST(read_ok);
        BOOST_TEST_EQ(in, out);
    }

    // io_uring's multishot accept can deliver a connection before any
    // accept() is outstanding to claim it; that connection's fd (and
    // tracking node) parks in the acceptor impl's ready_fds_ list. If
    // the acceptor is destroyed with a parked fd still there, draining
    // it was originally destructor-only -- which never runs on the
    // recycle path (reaching zero references usually means "back on
    // the free list", not "actually freed"). Reproduces deterministically:
    // destroy an acceptor holding one buffered connection, then
    // construct a fresh acceptor on the same context -- that pops the
    // just-recycled impl and, before the fix, immediately failed
    // reuse()'s `ready_fds_.empty()` assert. On epoll/select there is
    // no ready_fds_/multishot concept, so this is a plain acceptor
    // recycle-and-reuse sanity check there; the bug this guards against
    // is uring-only.
    void testAcceptorRecycleDrainsBufferedConnection()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        {
            tcp_acceptor acc(ioc);
            BOOST_TEST(!acc.open());
            acc.set_option(socket_option::reuse_address(true));
            BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
            BOOST_TEST(!acc.listen());
            auto port = acc.local_endpoint().port();

            tcp_socket client(ioc);
            bool connected = false;
            capy::run_async(ex)(
                [](tcp_socket& s, endpoint ep, bool& done) -> capy::task<> {
                    auto [ec] = co_await s.connect(ep);
                    done      = !ec;
                }(client, endpoint(ipv4_address::loopback(), port),
                  connected));

            // No accept() is outstanding: on uring the connection's
            // multishot CQE buffers in ready_fds_ instead of completing
            // an accept. run() (not poll()) drives the connector fully
            // to completion, which is also where the server's accept
            // CQE lands -- same idiom as
            // multishot_acceptor_test::testDestroyWithBufferedConnections.
            ioc.run();
            BOOST_TEST(connected);

            client.close();

            // acc drops here holding a buffered connection in
            // ready_fds_ (uring) or nothing to hold (epoll/select).
        }

        ioc.restart();

        // A fresh acceptor on the same context pops the just-recycled
        // impl. Before the fix this aborted on reuse()'s
        // `ready_fds_.empty()` assert (uring only).
        tcp_acceptor acc2(ioc);
        BOOST_TEST(!acc2.open());
        acc2.set_option(socket_option::reuse_address(true));
        BOOST_TEST(!acc2.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc2.listen());
        auto port2 = acc2.local_endpoint().port();

        // Full exercise: accept a real connection through the recycled
        // (or freshly allocated) impl to prove it still works.
        tcp_socket server2(ioc);
        tcp_socket client2(ioc);
        bool accept_done = false, connect_done = false;
        capy::run_async(ex)(
            [](tcp_acceptor& a, tcp_socket& s, bool& done) -> capy::task<> {
                auto [ec] = co_await a.accept(s);
                done      = !ec;
            }(acc2, server2, accept_done));
        capy::run_async(ex)(
            [](tcp_socket& s, endpoint ep, bool& done) -> capy::task<> {
                auto [ec] = co_await s.connect(ep);
                done      = !ec;
            }(client2, endpoint(ipv4_address::loopback(), port2),
              connect_done));
        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(connect_done);
        BOOST_TEST(server2.is_open());
    }

    void run()
    {
        testAbandonedReadThenReuse();
        testAcceptorRecycleDrainsBufferedConnection();
    }
};

// Churns every pooled object kind through 32 construct/destroy cycles,
// then runs one representative op per kind so the exercise below runs
// entirely on recycled (not freshly allocated) impls. Registered on
// the reactor backends plus io_uring -- not IOCP, which is covered by
// its own object_pool conversion separately.
template<auto Backend>
struct churn_then_exercise_test
{
    static constexpr int churn_n = 32;

    void churnSockets(io_context& ioc)
    {
        for (int i = 0; i < churn_n; ++i)
        {
            tcp_socket s(ioc);
        }
    }

    void churnAcceptors(io_context& ioc)
    {
        for (int i = 0; i < churn_n; ++i)
        {
            tcp_acceptor a(ioc);
        }
    }

    void churnUdp(io_context& ioc)
    {
        for (int i = 0; i < churn_n; ++i)
        {
            udp_socket u(ioc);
        }
    }

    void churnResolvers(io_context& ioc)
    {
        for (int i = 0; i < churn_n; ++i)
        {
            resolver r(ioc);
        }
    }

    void churnSignalSets(io_context& ioc)
    {
#if BOOST_COROSIO_POSIX
        for (int i = 0; i < churn_n; ++i)
        {
            signal_set s(ioc, SIGUSR1);
        }
#else
        // SIGUSR1 doesn't exist on Windows; this template is never
        // instantiated there (COROSIO_NON_IOCP_BACKEND_TESTS registers
        // nothing on Windows), but non-dependent names inside it must
        // still resolve at definition time regardless of instantiation.
        (void)ioc;
#endif
    }

    void churnFiles(io_context& ioc)
    {
        test::temp_file tmp("object_reuse_churn_");
        for (int i = 0; i < churn_n; ++i)
        {
            stream_file f(ioc);
            std::ignore = f.open(
                tmp.path,
                file_base::read_write | file_base::create |
                    file_base::truncate);
        }
    }

    void churnTimers(io_context& ioc, capy::executor_ref ex)
    {
        for (int i = 0; i < churn_n; ++i)
        {
            bool done = false;
            capy::run_async(ex)([](bool& d) -> capy::task<> {
                auto [ec] = co_await delay(std::chrono::milliseconds(0));
                d         = !ec;
            }(done));
            ioc.run();
            ioc.restart();
            BOOST_TEST(done);
        }
    }

    // socket + acceptor: accept, connect, read, write, then cancel a
    // fresh read via stop_token -- every op the brief names, run on
    // the impls churnSockets()/churnAcceptors() just recycled.
    void exerciseSocketAndAcceptor(io_context& ioc, capy::executor_ref ex)
    {
        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        acc.set_option(socket_option::reuse_address(true));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        auto port = acc.local_endpoint().port();

        tcp_socket server(ioc);
        tcp_socket client(ioc);
        bool accept_done = false, connect_done = false;

        capy::run_async(ex)(
            [](tcp_acceptor& a, tcp_socket& s, bool& done) -> capy::task<> {
                auto [ec] = co_await a.accept(s);
                done      = !ec;
            }(acc, server, accept_done));
        capy::run_async(ex)(
            [](tcp_socket& s, endpoint ep, bool& done) -> capy::task<> {
                auto [ec] = co_await s.connect(ep);
                done      = !ec;
            }(client, endpoint(ipv4_address::loopback(), port),
              connect_done));
        ioc.run();
        ioc.restart();
        BOOST_TEST(accept_done);
        BOOST_TEST(connect_done);

        char const out = 'q';
        char in        = 0;
        bool wrote = false, read_ok = false;
        capy::run_async(ex)(
            [](tcp_socket& s, char const* b, bool& done) -> capy::task<> {
                auto [ec, n] = co_await s.write_some(capy::const_buffer(b, 1));
                done         = !ec && n == 1;
            }(client, &out, wrote));
        capy::run_async(ex)(
            [](tcp_socket& s, char* b, bool& done) -> capy::task<> {
                auto [ec, n] = co_await s.read_some(capy::mutable_buffer(b, 1));
                done         = !ec && n == 1;
            }(server, &in, read_ok));
        ioc.run();
        ioc.restart();
        BOOST_TEST(wrote);
        BOOST_TEST(read_ok);
        BOOST_TEST_EQ(in, out);

        // Cancel-via-stop_token: park a second read with no writer,
        // then cancel it through the token instead of close()/
        // destroy() -- a different code path through the same
        // recycled impl's op machinery.
        std::stop_source src;
        bool canceled = false;
        capy::run_async(ex, src.get_token())(
            [](tcp_socket& s, bool& done) -> capy::task<> {
                char buf[1];
                auto [ec, n] =
                    co_await s.read_some(capy::mutable_buffer(buf, 1));
                std::ignore = n;
                done        = (ec == capy::cond::canceled);
            }(client, canceled));
        ioc.run_one();
        src.request_stop();
        ioc.run();
        ioc.restart();
        BOOST_TEST(canceled);

        client.close();
        server.close();
        acc.close();
    }

    // udp: send_to/recv_from on the recycled churnUdp() impls.
    void exerciseUdp(io_context& ioc, capy::executor_ref ex)
    {
        udp_socket receiver(ioc);
        BOOST_TEST(!receiver.open());
        BOOST_TEST(!receiver.bind(endpoint(ipv4_address::loopback(), 0)));
        auto rport = receiver.local_endpoint().port();

        udp_socket sender(ioc);
        BOOST_TEST(!sender.open());
        BOOST_TEST(!sender.bind(endpoint(ipv4_address::loopback(), 0)));

        char const out = 'u';
        char in        = 0;
        bool sent = false, received = false;
        endpoint dest(ipv4_address::loopback(), rport);

        capy::run_async(ex)(
            [](udp_socket& s, char const* b, endpoint ep,
               bool& done) -> capy::task<> {
                auto [ec, n] =
                    co_await s.send_to(capy::const_buffer(b, 1), ep);
                done = !ec && n == 1;
            }(sender, &out, dest, sent));
        endpoint source;
        capy::run_async(ex)(
            [](udp_socket& s, char* b, endpoint& src,
               bool& done) -> capy::task<> {
                auto [ec, n] =
                    co_await s.recv_from(capy::mutable_buffer(b, 1), src);
                done = !ec && n == 1;
            }(receiver, &in, source, received));
        ioc.run();
        ioc.restart();

        BOOST_TEST(sent);
        BOOST_TEST(received);
        BOOST_TEST_EQ(in, out);
    }

    // resolver: resolve localhost on a recycled churnResolvers() impl.
    void exerciseResolver(io_context& ioc, capy::executor_ref ex)
    {
        resolver r(ioc);
        bool done = false;
        std::error_code resolve_ec;

        capy::run_async(ex)(
            [](resolver& r_ref, bool& done_out,
               std::error_code& ec_out) -> capy::task<> {
                auto [ec, results] = co_await r_ref.resolve("localhost", "80");
                std::ignore         = results;
                ec_out              = ec;
                done_out            = true;
            }(r, done, resolve_ec));
        ioc.run();
        ioc.restart();

        BOOST_TEST(done);
        BOOST_TEST(!resolve_ec);
    }

    void run()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        churnSockets(ioc);
        churnAcceptors(ioc);
        churnUdp(ioc);
        churnResolvers(ioc);
        churnSignalSets(ioc);
        churnFiles(ioc);
        churnTimers(ioc, ex);

        exerciseSocketAndAcceptor(ioc, ex);
        exerciseUdp(ioc, ex);
        exerciseResolver(ioc, ex);

        // signal_set: wait() completed by raise() on a recycled
        // churnSignalSets() impl.
#if BOOST_COROSIO_POSIX
        {
            signal_set sig(ioc, SIGUSR1);
            bool done = false;
            capy::run_async(ex)(
                [](signal_set& s, bool& done_out) -> capy::task<> {
                    [[maybe_unused]] auto [ec, signum] = co_await s.wait();
                    done_out                           = !ec;
                }(sig, done));
            ioc.run_one();
            std::raise(SIGUSR1);
            ioc.run();
            ioc.restart();
            BOOST_TEST(done);
        }
#endif

        // files: write_some on a recycled churnFiles() impl.
        {
            test::temp_file tmp("object_reuse_exercise_");
            stream_file f(ioc);
            BOOST_TEST(!f.open(
                tmp.path,
                file_base::read_write | file_base::create |
                    file_base::truncate));
            bool done = false;
            char const data = 'f';
            capy::run_async(ex)(
                [](stream_file& file, char const* b,
                   bool& done_out) -> capy::task<> {
                    auto [ec, n] =
                        co_await file.write_some(capy::const_buffer(b, 1));
                    done_out = !ec && n == 1;
                }(f, &data, done));
            ioc.run();
            ioc.restart();
            BOOST_TEST(done);
        }

        // timer: one more delay() wait on a recycled churnTimers() impl.
        {
            bool done = false;
            capy::run_async(ex)([](bool& d) -> capy::task<> {
                auto [ec] = co_await delay(std::chrono::milliseconds(0));
                d         = !ec;
            }(done));
            ioc.run();
            ioc.restart();
            BOOST_TEST(done);
        }
    }
};

COROSIO_BACKEND_TESTS(object_reuse_test, "boost.corosio.object_reuse")
COROSIO_NON_IOCP_BACKEND_TESTS(
    churn_then_exercise_test, "boost.corosio.object_reuse.churn")

// AF_UNIX churn lane. local_stream_socket/local_stream_acceptor/
// local_datagram_socket instantiate the exact same reactor_stream_
// socket/reactor_acceptor/reactor_datagram_socket templates patched
// above, just parameterized on local_endpoint instead of endpoint --
// local_endpoint_ is precisely the field poison() caught a missing
// reset for, and nothing else loops these types the way
// churn_then_exercise_test loops the IP-socket kinds. Windows
// loopback rules don't apply: this never binds an IP endpoint, only
// AF_UNIX paths, which are POSIX-only on epoll/select and also
// supported on Windows 10 1803+ -- irrelevant here since
// COROSIO_NON_IOCP_BACKEND_TESTS registers nothing on Windows anyway.
template<auto Backend>
struct local_socket_churn_then_exercise_test
{
    static constexpr int churn_n = 32;

    void churnStreamSockets(io_context& ioc)
    {
        for (int i = 0; i < churn_n; ++i)
        {
            local_stream_socket s(ioc);
        }
    }

    void churnAcceptors(io_context& ioc)
    {
        for (int i = 0; i < churn_n; ++i)
        {
            local_stream_acceptor a(ioc);
        }
    }

    void churnDatagramSockets(io_context& ioc)
    {
#if BOOST_COROSIO_POSIX
        for (int i = 0; i < churn_n; ++i)
        {
            local_datagram_socket u(ioc);
        }
#else
        // local_datagram_socket is POSIX-only (local_datagram_socket.hpp).
        // This template is never instantiated on Windows
        // (COROSIO_NON_IOCP_BACKEND_TESTS registers nothing there), but
        // non-dependent names inside it must still resolve at definition
        // time regardless of instantiation.
        (void)ioc;
#endif
    }

    // local_stream_socket + local_stream_acceptor: accept, connect,
    // echo one byte each way, then close -- the same shape as
    // churn_then_exercise_test's TCP exercise, run on the AF_UNIX
    // impls churnStreamSockets()/churnAcceptors() just recycled.
    void exerciseStreamAndAcceptor(io_context& ioc, capy::executor_ref ex)
    {
        test::temp_socket_dir dir;
        local_endpoint ep(dir.path());

        local_stream_acceptor acc(ioc, ep);
        BOOST_TEST(acc.is_open());

        local_stream_socket server(ioc);
        local_stream_socket client(ioc);
        bool accept_done = false, connect_done = false;

        capy::run_async(ex)(
            [](local_stream_acceptor& a, local_stream_socket& s,
               bool& done) -> capy::task<> {
                auto [ec] = co_await a.accept(s);
                done      = !ec;
            }(acc, server, accept_done));
        capy::run_async(ex)(
            [](local_stream_socket& s, local_endpoint ep_,
               bool& done) -> capy::task<> {
                auto [ec] = co_await s.connect(ep_);
                done      = !ec;
            }(client, ep, connect_done));
        ioc.run();
        ioc.restart();
        BOOST_TEST(accept_done);
        BOOST_TEST(connect_done);

        char const out = 'l';
        char in        = 0;
        bool wrote = false, read_ok = false;
        capy::run_async(ex)(
            [](local_stream_socket& s, char const* b,
               bool& done) -> capy::task<> {
                auto [ec, n] = co_await s.write_some(capy::const_buffer(b, 1));
                done         = !ec && n == 1;
            }(client, &out, wrote));
        capy::run_async(ex)(
            [](local_stream_socket& s, char* b, bool& done) -> capy::task<> {
                auto [ec, n] =
                    co_await s.read_some(capy::mutable_buffer(b, 1));
                done = !ec && n == 1;
            }(server, &in, read_ok));
        ioc.run();
        ioc.restart();
        BOOST_TEST(wrote);
        BOOST_TEST(read_ok);
        BOOST_TEST_EQ(in, out);

        client.close();
        server.close();
        acc.close();
    }

    // local_datagram_socket: connect_pair() + connected-mode send()/
    // recv(), on the recycled impls churnDatagramSockets() just
    // warmed.
    void exerciseDatagram(io_context& ioc, capy::executor_ref ex)
    {
#if BOOST_COROSIO_POSIX
        local_datagram_socket a(ioc);
        local_datagram_socket b(ioc);
        BOOST_TEST(!connect_pair(a, b));

        char const out = 'd';
        char in        = 0;
        bool sent = false, received = false;

        capy::run_async(ex)(
            [](local_datagram_socket& s, char const* buf,
               bool& done) -> capy::task<> {
                auto [ec, n] = co_await s.send(capy::const_buffer(buf, 1));
                done         = !ec && n == 1;
            }(a, &out, sent));
        capy::run_async(ex)(
            [](local_datagram_socket& s, char* buf,
               bool& done) -> capy::task<> {
                auto [ec, n] = co_await s.recv(capy::mutable_buffer(buf, 1));
                done         = !ec && n == 1;
            }(b, &in, received));
        ioc.run();
        ioc.restart();

        BOOST_TEST(sent);
        BOOST_TEST(received);
        BOOST_TEST_EQ(in, out);

        a.close();
        b.close();
#else
        (void)ioc;
        (void)ex;
#endif
    }

    void run()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        churnStreamSockets(ioc);
        churnAcceptors(ioc);
        churnDatagramSockets(ioc);

        exerciseStreamAndAcceptor(ioc, ex);
        exerciseDatagram(ioc, ex);
    }
};

COROSIO_NON_IOCP_BACKEND_TESTS(
    local_socket_churn_then_exercise_test,
    "boost.corosio.object_reuse.churn_local")

} // namespace boost::corosio
