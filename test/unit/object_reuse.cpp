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
#include <boost/corosio/detail/platform.hpp>
#if BOOST_COROSIO_POSIX
#include <boost/corosio/posix_stream_descriptor.hpp>
#include <fcntl.h>
#include <unistd.h>
#endif
#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/strand.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <system_error>
#include <tuple>

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
    // test's own assertions are the behavioral half: the abandoned read
    // completes, and the context keeps working afterward.
    void testAbandonedReadThenReuse()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        // Declared at function scope, not inside the block below: the
        // abandoned read's cancellation resumes the coroutine after the
        // block has exited, and it writes `resumed`.
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

        // Drain the cancellation the destructor produced.
        ioc.run();
        ioc.restart();
        BOOST_TEST(resumed);

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
    // tracking node) parks in the acceptor impl's ready_fds_ list.
    // Reaching zero references recycles the impl rather than freeing
    // it, so the parked fd must be drained on that path too: destroy
    // an acceptor holding one buffered connection, then construct a
    // fresh acceptor on the same context, which pops the recycled impl
    // and asserts `ready_fds_.empty()` in reuse(). On epoll/select
    // there is no ready_fds_/multishot concept, so this is a plain
    // acceptor recycle-and-reuse check there.
    // A socket destroyed while its connect is in flight must not have the
    // connect's completion write endpoints into it: the impl is recycled
    // and the next socket would report the old peer.
    void testConnectThenDestroyRecycles()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        acc.set_option(socket_option::reuse_address(true));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        endpoint ep(ipv4_address::loopback(), acc.local_endpoint().port());

        for (int i = 0; i < 8; ++i)
        {
            bool started = false;
            {
                tcp_socket s(ioc);
                BOOST_TEST(!s.open());
                capy::run_async(ex)(
                    [](tcp_socket& sock, endpoint e,
                       bool& began) -> capy::task<> {
                        began       = true;
                        std::ignore = co_await sock.connect(e);
                    }(s, ep, started));
                ioc.restart();
                while (!started && ioc.poll_one() != 0)
                {
                }
            }
            ioc.restart();
            ioc.run();

            tcp_socket fresh(ioc);
            BOOST_TEST(!fresh.open());
            BOOST_TEST(fresh.remote_endpoint() == endpoint{});
        }
    }

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

        // On uring the impl recycles only once the cancelled arming's
        // terminal CQE is reaped, and poll() pumps the ring only while
        // work is outstanding.
        for (int i = 0; i < 8; ++i)
        {
            capy::run_async(ex)([]() -> capy::task<> { co_return; }());
            ioc.restart();
            ioc.poll();
        }
        ioc.restart();

        // A fresh acceptor on the same context pops the just-recycled
        // impl, whose reuse() asserts `ready_fds_.empty()` (uring only).
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

    // A completion resumed through a strand is queued there by
    // reference. Destroying the socket recycles its impl while that
    // entry waits, and a new socket may reuse the impl and complete an
    // operation of its own before the strand reaches the entry. The
    // original reader must still be resumed, once, with its own result.
    void testStrandCompletionOutlivesRecycle()
    {
        io_context ioc(Backend);
        capy::strand st(ioc.get_executor());
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);
        auto sock = std::make_unique<tcp_socket>(std::move(s1));
        auto [q1, q2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);
        ioc.restart();

        int resumed = 0;
        std::error_code rec;
        char buf[1];
        capy::run_async(st)(
            [](tcp_socket& s, char* b, int& n,
               std::error_code& ec) -> capy::task<> {
                auto [e, k] = co_await s.read_some(capy::mutable_buffer(b, 1));
                std::ignore = k;
                ec          = e;
                ++n;
            }(*sock, buf, resumed, rec));

        std::optional<tcp_socket> fresh;
        bool reused_done = false;
        char pbuf[1];
        auto reuse = [](io_context& ioc, std::optional<tcp_socket>& f,
                        native_handle_type h, char* pb,
                        bool& done) -> capy::task<> {
            f.emplace(ioc);
            BOOST_TEST(!f->assign(h));
            auto [e, k] = co_await f->read_some(capy::mutable_buffer(pb, 1));
            BOOST_TEST(!e);
            BOOST_TEST_EQ(k, 1u);
            done = true;
        };

        // Destroying the socket queues the reader's completion ahead of
        // the strand's turn, so the strand holds the reuse first and the
        // reader behind it. The byte written first lets the reuse's read
        // complete without waiting.
        capy::run_async(ioc.get_executor())(
            [](capy::strand<io_context::executor_type> st,
               std::unique_ptr<tcp_socket>& sock, tcp_socket& peer,
               capy::task<> reuse) -> capy::task<> {
                char c      = 'x';
                auto [e, k] = co_await peer.write_some(capy::const_buffer(&c, 1));
                BOOST_TEST(!e);
                BOOST_TEST_EQ(k, 1u);
                sock.reset();
                capy::run_async(st)(std::move(reuse));
            }(st, sock, q2, reuse(ioc, fresh, q1.release(), pbuf, reused_done)));

        ioc.run();
        BOOST_TEST_EQ(resumed, 1);
        BOOST_TEST(rec == capy::cond::canceled);
        BOOST_TEST(reused_done);
    }

    void run()
    {
        testAbandonedReadThenReuse();
        testConnectThenDestroyRecycles();
        testAcceptorRecycleDrainsBufferedConnection();
        testStrandCompletionOutlivesRecycle();
    }
};

// Churns every pooled object kind through 32 construct/destroy cycles,
// then runs one representative op per kind so the exercise below runs
// entirely on recycled (not freshly allocated) impls. Registered on
// the reactor backends plus io_uring; IOCP has no churn coverage yet.
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
        std::ignore = ioc;
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
                auto [ec] = co_await delay(std::chrono::microseconds(1));
                d         = !ec;
            }(done));
            ioc.run();
            ioc.restart();
            BOOST_TEST(done);
        }
    }

    // socket + acceptor: accept, connect, read, write, then cancel a
    // fresh read via stop_token, run on the impls
    // churnSockets()/churnAcceptors() just recycled.
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
                auto [ec] = co_await delay(std::chrono::microseconds(1));
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
        std::ignore = ioc;
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
        std::ignore = ioc;
        std::ignore = ex;
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

#if BOOST_COROSIO_POSIX
// Descriptor lane: each cycle abandons a parked read on a recycled
// descriptor impl, so a reset reuse() forgets surfaces as the poison
// pattern (debug) or a stale op on the next cycle's impl.
template<auto Backend>
struct descriptor_churn_then_exercise_test
{
    static constexpr int cycles = 16;

    void run()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        for (int i = 0; i < cycles; ++i)
        {
            int fds[2];
            BOOST_TEST_EQ(::pipe(fds), 0);
            std::error_code rec;
            bool done = false;
            char buf[4];
            {
                posix_stream_descriptor d(ioc);
                BOOST_TEST(!d.assign(fds[0]));
                capy::run_async(ex)(
                    [](posix_stream_descriptor& d,
                       char* b,
                       std::error_code& ec,
                       bool& done) -> capy::task<> {
                        auto [e, n] =
                            co_await d.read_some(capy::mutable_buffer(b, 4));
                        std::ignore = n;
                        ec          = e;
                        done        = true;
                    }(d, buf, rec, done));
                ioc.poll();
                d.cancel();
                ioc.run();
                ioc.restart();
            }
            BOOST_TEST(done);
            BOOST_TEST(rec == capy::cond::canceled);
            ::close(fds[1]);
        }

        // Closing a recycled impl that was never assigned tears down
        // whatever reuse() left in its descriptor state.
        {
            posix_stream_descriptor unused(ioc);
            unused.cancel();
        }
        ioc.poll();
        ioc.restart();

        int fds[2];
        BOOST_TEST_EQ(::pipe(fds), 0);
        posix_stream_descriptor d(ioc);
        BOOST_TEST(!d.assign(fds[0]));
        BOOST_TEST_EQ(::write(fds[1], "abc", 3), 3);
        std::size_t got = 0;
        char buf[4];
        capy::run_async(ex)(
            [](posix_stream_descriptor& d,
               char* b,
               std::size_t& got) -> capy::task<> {
                auto [e, n] = co_await d.read_some(capy::mutable_buffer(b, 4));
                if (!e)
                    got = n;
            }(d, buf, got));
        ioc.run();
        BOOST_TEST_EQ(got, 3u);
        ::close(fds[1]);
    }
};

COROSIO_NON_IOCP_BACKEND_TESTS(
    descriptor_churn_then_exercise_test,
    "boost.corosio.object_reuse.churn_descriptor")

// A close hands the reference covering a still-queued reactor
// invocation to that invocation. Re-adopting a descriptor the reactor
// cannot watch, then closing it again, must not drop that reference:
// the invocation would run on an impl already recycled to a new owner.
template<auto Backend>
struct descriptor_requeue_test
{
    void run()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        int fds[2];
        BOOST_TEST_EQ(::pipe(fds), 0);
        auto d = std::make_unique<posix_stream_descriptor>(ioc);
        BOOST_TEST(!d->assign(fds[0]));
        BOOST_TEST_EQ(::write(fds[1], "x", 1), 1);

        // Posted ahead of the first reactor pass, so it runs after the
        // pass has queued the descriptor's invocation and before that
        // invocation does.
        bool ran = false;
        capy::run_async(ex)(
            [](std::unique_ptr<posix_stream_descriptor>& d,
               io_context& ioc,
               bool& ran) -> capy::task<> {
                d->close();
                int nul = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                std::ignore = d->assign(nul);
                d.reset();
                // Pops the recycled impl if the invocation lost its
                // reference; its reuse() asserts nothing is queued.
                posix_stream_descriptor fresh(ioc);
                ran = true;
                co_return;
            }(d, ioc, ran));
        ioc.run();
        BOOST_TEST(ran);
        ::close(fds[1]);
    }
};

COROSIO_NON_IOCP_BACKEND_TESTS(
    descriptor_requeue_test, "boost.corosio.object_reuse.descriptor_requeue")
#endif

} // namespace boost::corosio
