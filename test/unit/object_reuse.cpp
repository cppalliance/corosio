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
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstddef>
#include <system_error>

#include "context.hpp"
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

COROSIO_BACKEND_TESTS(object_reuse_test, "boost.corosio.object_reuse")

} // namespace boost::corosio
