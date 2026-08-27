//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_FAULT_REACTOR_FAULTS_HPP
#define BOOST_COROSIO_TEST_FAULT_REACTOR_FAULTS_HPP

#include "fault.hpp"
#include "fault_test_utils.hpp"
#include "context.hpp"
#include "test_suite.hpp"

#include <boost/corosio/delay.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <stop_token>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/select.h>
#include <sys/socket.h>

namespace boost::corosio::test::fault {

/* Fault tests that hold for every reactor backend.

   The syscalls these arm live in the shared reactor sources
   (reactor_op.hpp, reactor_descriptor_state.hpp,
   reactor_stream_socket.hpp, reactor_datagram_socket.hpp) or in
   traits that spell them identically on epoll and select, so one body
   covers both. Backend-specific registration and run-loop faults stay
   in the per-backend files.
*/
template<auto Backend>
struct reactor_common_faults
{
#if BOOST_COROSIO_HAS_SELECT
    static constexpr bool is_select = std::is_same_v<
        std::remove_cvref_t<decltype(Backend)>, select_t>;
#else
    static constexpr bool is_select = false;
#endif

#if BOOST_COROSIO_HAS_KQUEUE
    static constexpr bool is_kqueue = std::is_same_v<
        std::remove_cvref_t<decltype(Backend)>, kqueue_t>;
#else
    static constexpr bool is_kqueue = false;
#endif

    // Every backend routes a write through its own write_policy, so the
    // symbol to arm differs. kqueue spells write_policy::write_one
    // write() and write_policy::write writev(); epoll and select spell
    // the same two send(MSG_NOSIGNAL) and sendmsg(), falling back to
    // write() where the platform has no MSG_NOSIGNAL. Darwin defines
    // it, so select on macOS arms exactly as it does on Linux.
#if defined(MSG_NOSIGNAL)
    static constexpr sys spec_write = is_kqueue ? sys::write : sys::send;
#else
    static constexpr sys spec_write = sys::write;
#endif
    static constexpr sys vec_write =
        is_kqueue ? sys::writev : sys::sendmsg;

    static endpoint loopback()
    {
        return endpoint(ipv4_address::loopback(), 0);
    }

    /* Connect a pair whose sender cannot outrun its peer.

       The send buffer has to be shrunk before the handshake: Darwin
       auto-tunes an established socket, so a size set afterwards no
       longer binds (wait.cpp, make_backpressured_pair). The accepted socket
       inherits the listener's, so both ends are shrunk here. The
       receive buffers keep their defaults: shrinking those closes the
       TCP window, and the parked write then waits on the persist timer
       rather than on the reader, which costs seconds on a backend that
       has no latched write readiness to short-circuit it.
    */
    static std::pair<tcp_socket, tcp_socket>
    make_backpressured_pair(io_context& ioc)
    {
        auto ex = ioc.get_executor();
        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        acc.set_option(socket_option::reuse_address(true));
        acc.set_option(socket_option::send_buffer_size(1024));
        // set_option throws rather than reporting, so reaching here
        // only proves the call was accepted. Every kernel clamps and
        // rescales the request, so read the effective size back: what
        // the caller depends on is that it is far below the payload,
        // not that it is 1 KiB.
        BOOST_TEST(
            acc.get_option<socket_option::send_buffer_size>().value() <
            64 * 1024);
        BOOST_TEST(!acc.bind(loopback()));
        BOOST_TEST(!acc.listen());
        auto port = acc.local_endpoint().port();

        tcp_socket s1(ioc), s2(ioc);
        BOOST_TEST(!s2.open(tcp::v4()));
        s2.set_option(socket_option::send_buffer_size(1024));
        BOOST_TEST(
            s2.get_option<socket_option::send_buffer_size>().value() <
            64 * 1024);

        std::error_code aec, cec;
        auto accept_task = [&]() -> capy::task<>
        {
            auto [ec] = co_await acc.accept(s1);
            aec = ec;
        };
        auto connect_task = [&]() -> capy::task<>
        {
            auto [ec] = co_await s2.connect(
                endpoint(ipv4_address::loopback(), port));
            cec = ec;
        };
        capy::run_async(ex)(accept_task());
        capy::run_async(ex)(connect_task());
        ioc.run();
        ioc.restart();
        BOOST_TEST(!aec);
        BOOST_TEST(!cec);
        return {std::move(s1), std::move(s2)};
    }

    void testAcceptorFails()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);
        {
            fault_scope f(sys::socket, EMFILE);
            BOOST_TEST(acc.open() == std::errc::too_many_files_open);
            BOOST_TEST(f.fired());
        }
        BOOST_TEST(!acc.open());
        {
            fault_scope f(sys::bind, EACCES);
            BOOST_TEST(acc.bind(loopback()) == std::errc::permission_denied);
            BOOST_TEST(f.fired());
        }
        BOOST_TEST(!acc.bind(loopback()));
        {
            fault_scope f(sys::listen, EADDRINUSE);
            BOOST_TEST(acc.listen() == std::errc::address_in_use);
            BOOST_TEST(f.fired());
        }
        // Not latched: the failed listen left the acceptor open and
        // unregistered, so a second listen still succeeds.
        BOOST_TEST(!acc.listen());
        {
            int before = open_fds();
            fault_scope f(sys::socket, EMFILE);
            expect_system_error(
                [&]{ tcp_acceptor a2(ioc, loopback()); },
                std::errc::too_many_files_open);
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(open_fds(), before);
        }
    }

    void testConnectFails()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc, loopback());
        // A faulted EINPROGRESS parks the connect op on a descriptor
        // the real connect() never touched. BSD never reports such a
        // socket as writable -- sowriteable() requires SS_ISCONNECTED
        // for a stream socket -- so neither kqueue nor select would
        // ever dispatch the parked op there, while Linux raises
        // POLLOUT on it immediately. Park the two deferred cases on
        // sockets that are already connected, which are writable
        // everywhere; the faulted connect still decides the result,
        // because the real syscall never runs. On a connected socket
        // register_op may find write_ready already latched and run
        // perform_io() inline, so what these blocks prove is that the
        // probe and the SO_ERROR read in reactor_connect_op::perform_io
        // are reached -- not that a reactor dispatch delivered them.
        auto [d1, peer1] = test::make_socket_pair(ioc);
        auto [d2, peer2] = test::make_socket_pair(ioc);
        std::error_code sync_ec, poll_ec, soerr_ec;
        auto body = [&]() -> capy::task<>
        {
            {
                tcp_socket s(ioc);
                fault_scope f(sys::connect, ENETUNREACH);
                auto [ec] = co_await s.connect(acc.local_endpoint());
                sync_ec = ec;
                BOOST_TEST(f.fired());
            }
            {
                // Loopback connect completes synchronously, so the
                // deferred path is only reachable by making connect
                // report EINPROGRESS.
                fault_scope f1(sys::connect, EINPROGRESS);
                fault_scope f2(sys::poll, EIO);
                auto [ec] = co_await d1.connect(acc.local_endpoint());
                poll_ec = ec;
                BOOST_TEST(f1.fired());
                BOOST_TEST(f2.fired());
            }
            {
                fault_scope f1(sys::connect, EINPROGRESS);
                fault_scope f2(sys::getsockopt, EBADF);
                auto [ec] = co_await d2.connect(acc.local_endpoint());
                soerr_ec = ec;
                BOOST_TEST(f1.fired());
                BOOST_TEST(f2.fired());
            }
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        // Named only so the structured binding is complete; what
        // matters is the scope, which keeps each peer open across the
        // run. A closed peer would reset the connection and make the
        // parked socket writable for the wrong reason.
        std::ignore = peer1;
        std::ignore = peer2;
        BOOST_TEST(sync_ec == std::errc::network_unreachable);
        BOOST_TEST(poll_ec == std::errc::io_error);
        BOOST_TEST(soerr_ec == std::errc::bad_file_descriptor);
    }

    void testStreamIoFails()
    {
        io_context ioc(Backend);
        auto [a, b] = test::make_socket_pair(ioc);
        char buf[8] = "1234567";
        std::error_code rec, wec, rec_multi, wec_multi, drec;
        auto body = [&]() -> capy::task<>
        {
            {
                fault_scope f(spec_write, EPIPE);
                auto [ec, n] = co_await a.write_some(
                    capy::const_buffer(buf, 7));
                std::ignore = n;
                wec = ec;
                BOOST_TEST(f.fired());
            }
            {
                fault_scope f(spec_write, EINTR);
                auto [ec, n] = co_await a.write_some(
                    capy::const_buffer(buf, 7));
                BOOST_TEST(f.fired());
                BOOST_TEST(!ec);
                BOOST_TEST_EQ(n, 7u);
            }
            {
                fault_scope f(sys::recv, ECONNRESET);
                auto [ec, n] = co_await b.read_some(
                    capy::mutable_buffer(buf, 7));
                std::ignore = n;
                rec = ec;
                BOOST_TEST(f.fired());
            }
            {
                fault_scope f(sys::recv, EINTR);
                auto [ec, n] = co_await b.read_some(
                    capy::mutable_buffer(buf, 7));
                BOOST_TEST(f.fired());
                BOOST_TEST(!ec);
                BOOST_TEST_EQ(n, 7u);
            }
            char x[4] = {}, y[4] = {};
            std::array<capy::mutable_buffer, 2> mb{
                capy::mutable_buffer(x, 4), capy::mutable_buffer(y, 4)};
            std::array<capy::const_buffer, 2> cb{
                capy::const_buffer(x, 4), capy::const_buffer(y, 4)};
            {
                fault_scope f(vec_write, EPIPE);
                auto [ec, n] = co_await a.write_some(cb);
                std::ignore = n;
                wec_multi = ec;
                BOOST_TEST(f.fired());
            }
            {
                fault_scope f(vec_write, EINTR);
                auto [ec, n] = co_await a.write_some(cb);
                BOOST_TEST(f.fired());
                BOOST_TEST(!ec);
                BOOST_TEST_EQ(n, 8u);
            }
            {
                auto [ec, n] = co_await a.write_some(cb);
                BOOST_TEST(!ec);
                BOOST_TEST_EQ(n, 8u);
            }
            {
                fault_scope f(sys::readv, ECONNRESET);
                auto [ec, n] = co_await b.read_some(mb);
                std::ignore = n;
                rec_multi = ec;
                BOOST_TEST(f.fired());
            }
            {
                fault_scope f(sys::readv, EINTR);
                auto [ec, n] = co_await b.read_some(mb);
                BOOST_TEST(f.fired());
                BOOST_TEST(!ec);
                BOOST_TEST_EQ(n, 8u);
            }
            {
                auto [ec, n] = co_await b.read_some(mb);
                BOOST_TEST(!ec);
                BOOST_TEST_EQ(n, 8u);
            }
            // Deferred read: the speculative recv reports EAGAIN, so the
            // reactor re-runs the op, which always uses readv even for a
            // single buffer.
            {
                fault_scope f1(sys::recv, EAGAIN);
                fault_scope f2(sys::readv, EIO);
                auto writer = [&]() -> capy::task<>
                {
                    auto [ec, n] = co_await a.write_some(
                        capy::const_buffer(buf, 4));
                    std::ignore = n;
                    BOOST_TEST(!ec);
                };
                capy::run_async(ioc.get_executor())(writer());
                auto [ec, n] = co_await b.read_some(
                    capy::mutable_buffer(buf, 7));
                std::ignore = n;
                drec = ec;
                BOOST_TEST(f1.fired());
                BOOST_TEST(f2.fired());
            }
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST(wec == std::errc::broken_pipe);
        BOOST_TEST(rec == std::errc::connection_reset);
        BOOST_TEST(wec_multi == std::errc::broken_pipe);
        BOOST_TEST(rec_multi == std::errc::connection_reset);
        BOOST_TEST(drec == std::errc::io_error);
    }

    void testDeferredWriteFails()
    {
        io_context ioc(Backend);
        // A faulted EAGAIN cannot park a write durably: the socket
        // stays writable, so an edge-triggered backend never fires
        // again and the op would never be retried. Push real
        // backpressure instead, so draining the peer produces a
        // genuine writable event.
        auto [a, b] = make_backpressured_pair(ioc);

        // The writer only parks once the peer's receive window is
        // closed as well, so the payload has to outrun both buffers:
        // the send buffer is pinned small above, but the receive
        // buffer keeps its default and Darwin autotunes it into the
        // hundreds of kilobytes. 256 KiB sat right at that boundary
        // and parked only sometimes.
        std::vector<char> payload(4 * 1024 * 1024, 'X');
        std::error_code wec;
        // The speculative single-buffer write takes the write_one fast
        // path, so the vector form is reachable only from the parked
        // op's reactor retry (reactor_write_op::perform_io).
        fault_scope f(vec_write, EIO);
        auto writer = [&]() -> capy::task<>
        {
            std::size_t off = 0;
            while(off < payload.size())
            {
                auto [ec, n] = co_await a.write_some(capy::const_buffer(
                    payload.data() + off, payload.size() - off));
                if(ec)
                {
                    wec = ec;
                    break;
                }
                off += n;
            }
            a.close();
        };
        auto reader = [&]() -> capy::task<>
        {
            std::vector<char> sink(4096);
            for(;;)
            {
                auto [ec, n] = co_await b.read_some(
                    capy::mutable_buffer(sink.data(), sink.size()));
                if(ec || n == 0)
                    break;
            }
        };
        capy::run_async(ioc.get_executor())(writer());
        capy::run_async(ioc.get_executor())(reader());
        ioc.run();
        BOOST_TEST(f.fired());
        BOOST_TEST(wec == std::errc::io_error);
    }

    void testShutdownAndWaitFails()
    {
        io_context ioc(Backend);
        // b is unused beyond keeping the peer of a alive.
        auto [a, b] = test::make_socket_pair(ioc);
        std::ignore = b;
        {
            fault_scope f(sys::shutdown, ENOTCONN);
            BOOST_TEST(a.shutdown(shutdown_send) == std::errc::not_connected);
            BOOST_TEST(f.fired());
        }
        std::error_code wec;
        auto body = [&]() -> capy::task<>
        {
            fault_scope f(sys::poll, EIO);
            auto [ec] = co_await a.wait(wait_type::write);
            wec = ec;
            BOOST_TEST(f.fired());
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST(wec == std::errc::io_error);
    }

    /* Fill the send path until it refuses a small write.

       A write-direction operation parks only on a descriptor the
       kernel would refuse, and how much a socket takes before it
       refuses is the kernel's business. Neither a `send` that stops
       short nor a descriptor that has stopped reporting itself
       writable is enough: a stream socket still accepts a few bytes
       below its low-water mark, and the operation these tests park is
       seven of them. The bulk loop saturates both ends — the peer
       never reads, so what stops it is the closed window — and the
       small sends that follow are the actual precondition, so the
       caller can skip rather than fail an unrelated assertion where
       filling does not close it.

       @return True once a small write would be refused.
    */
    static bool fill_send_buffer(tcp_socket& s)
    {
        std::vector<char> chunk(64 * 1024, 'x');
        int const fd = static_cast<int>(s.native_handle());
        // A refused send is not the end of the loop: the peer's receive
        // buffer keeps draining ours, so the window reopens until that
        // is full too.
        for(int i = 0; i < 512; ++i)
            std::ignore = ::send(fd, chunk.data(), chunk.size(), MSG_DONTWAIT);
        for(int i = 0; i < 4096; ++i)
        {
            if(::send(fd, chunk.data(), 8, MSG_DONTWAIT) < 0)
                return true;
        }
        return false;
    }

    /* Empty a peer's receive buffer so the sender's window reopens.

       The counterpart to fill_send_buffer: the sender is blocked on a
       closed window rather than on its own buffer, so what reopens it
       is the far end reading.
    */
    static void drain_receive_buffer(tcp_socket& s)
    {
        std::vector<char> buf(64 * 1024);
        int const fd = static_cast<int>(s.native_handle());
        while(::recv(fd, buf.data(), buf.size(), MSG_DONTWAIT) > 0)
        {
        }
    }

    // Report a kernel this test cannot put into the state it needs.
    static void skip_unfillable(char const* what)
    {
        std::fprintf(stderr,
            "fault harness: the send window would not stay closed on "
            "this kernel; skipping %s\n", what);
    }

    /* Raise an error condition on the far end of `peer`.

       epoll and kqueue report a reset as an error event, so SO_LINGER
       0 plus a close is enough. select reports an exceptional
       condition only for out-of-band data, which is why the two
       backends need different triggers for the same dispatch arm.
    */
    static void raise_error_condition(tcp_socket& peer)
    {
        // A parked operation that resolved early leaves nothing here to
        // raise the condition on. Report that rather than throwing out
        // of a coroutine, where it would abort the process and hide
        // whichever assertion actually failed.
        BOOST_TEST(peer.is_open());
        if(!peer.is_open())
            return;
        if constexpr(is_select)
        {
            char oob = '!';
            BOOST_TEST_EQ(
                ::send(peer.native_handle(), &oob, 1, MSG_OOB), 1);
        }
        else
        {
            peer.set_option(socket_option::linger(true, 0));
            peer.close();
        }
    }

    /* An error arriving on a descriptor with an operation parked on it.

       The dispatch has a separate arm per parked op kind, each of
       which completes the op with the error instead of running its
       I/O. The SO_ERROR probe is faulted so what the op reports is the
       armed code rather than whatever the kernel recorded — the same
       trick testErrorEventSoError uses for the plain read arm.
    */
    void testErrorEventOnParkedWaitRead()
    {
        io_context ioc(Backend);
        auto [c, peer] = test::make_socket_pair(ioc);
        std::stop_source guard;
        std::error_code wec;
        auto waiter = [&]() -> capy::task<>
        {
            fault_scope probe(sys::getsockopt, EBADF);
            auto [ec] = co_await c.wait(wait_type::read);
            wec = ec;
            BOOST_TEST(probe.fired());
            // An out-of-band byte keeps select's except set raised, so
            // the socket that raised it goes before the next pass.
            peer.close();
            guard.request_stop();
        };
        auto trigger = [&]() -> capy::task<>
        {
            std::ignore = co_await corosio::delay(
                std::chrono::milliseconds(1));
            raise_error_condition(peer);
        };
        bool expired = false;
        capy::run_async(ioc.get_executor())(waiter());
        capy::run_async(ioc.get_executor())(trigger());
        capy::run_async(ioc.get_executor(), guard.get_token())(
            stop_guard(ioc, expired));
        ioc.run();
        BOOST_TEST(!expired);
        BOOST_TEST(wec == std::errc::bad_file_descriptor);
        BOOST_TEST(c.is_open());
    }

    void testErrorEventOnParkedWaitError()
    {
        io_context ioc(Backend);
        auto [c, peer] = test::make_socket_pair(ioc);
        std::stop_source guard;
        std::error_code wec;
        auto waiter = [&]() -> capy::task<>
        {
            fault_scope probe(sys::getsockopt, EBADF);
            auto [ec] = co_await c.wait(wait_type::error);
            wec = ec;
            BOOST_TEST(probe.fired());
            peer.close();
            guard.request_stop();
        };
        auto trigger = [&]() -> capy::task<>
        {
            std::ignore = co_await corosio::delay(
                std::chrono::milliseconds(1));
            raise_error_condition(peer);
        };
        bool expired = false;
        capy::run_async(ioc.get_executor())(waiter());
        capy::run_async(ioc.get_executor())(trigger());
        capy::run_async(ioc.get_executor(), guard.get_token())(
            stop_guard(ioc, expired));
        ioc.run();
        BOOST_TEST(!expired);
        BOOST_TEST(wec == std::errc::bad_file_descriptor);
        BOOST_TEST(c.is_open());
    }

    /* The two write-direction arms, on Linux only.

       Both need an operation that is still parked when the error
       arrives, so the trigger runs in the run-loop turn straight after
       the one that parked it, with nothing posted in between; a socket
       whose send window can reopen on its own does not stay parked any
       longer than that. Where it resolves even in that gap the test
       reports a skip rather than asserting on a dispatch that never
       ran.

       The BSD family never reaches the arm at all. A reset delivered
       to a parked write surfaces there as writability rather than as
       an error condition, on kqueue and on select alike, so the op
       re-runs its I/O and reports the real error instead of the
       faulted probe's. The trigger that does raise an error condition
       on the write side is testErrorEventOnWritableWrite's, which runs
       everywhere and covers the same two arms; what these two add is
       the reset, and only where a reset reaches them.
    */
    void testErrorEventOnParkedWrite()
    {
        io_context ioc(Backend);
        auto [c, peer] = make_backpressured_pair(ioc);
        if(!fill_send_buffer(c))
        {
            skip_unfillable("testErrorEventOnParkedWrite");
            return;
        }
        char buf[8] = "1234567";
        std::stop_source guard;
        std::error_code wec;
        bool done = false, parked = false, probe_fired = false;
        auto writer = [&]() -> capy::task<>
        {
            fault_scope probe(sys::getsockopt, EBADF);
            auto [ec, n] = co_await c.write_some(
                capy::const_buffer(buf, 7));
            std::ignore = n;
            wec = ec;
            probe_fired = probe.fired();
            done = true;
            peer.close();
            guard.request_stop();
        };
        auto trigger = [&]() -> capy::task<>
        {
            parked = !done;
            if(parked)
                raise_error_condition(peer);
            co_return;
        };
        bool expired = false;
        capy::run_async(ioc.get_executor())(writer());
        capy::run_async(ioc.get_executor())(trigger());
        capy::run_async(ioc.get_executor(), guard.get_token())(
            stop_guard(ioc, expired));
        ioc.run();
        BOOST_TEST(!expired);
        if(!parked)
        {
            skip_unfillable("testErrorEventOnParkedWrite");
            return;
        }
        BOOST_TEST(probe_fired);
        BOOST_TEST(wec == std::errc::bad_file_descriptor);
        BOOST_TEST(c.is_open());
    }

    void testErrorEventOnParkedWaitWrite()
    {
        io_context ioc(Backend);
        auto [c, peer] = make_backpressured_pair(ioc);
        if(!fill_send_buffer(c))
        {
            skip_unfillable("testErrorEventOnParkedWaitWrite");
            return;
        }
        std::stop_source guard;
        std::error_code wec;
        bool done = false, parked = false, probe_fired = false;
        auto waiter = [&]() -> capy::task<>
        {
            fault_scope probe(sys::getsockopt, EBADF);
            auto [ec] = co_await c.wait(wait_type::write);
            wec = ec;
            probe_fired = probe.fired();
            done = true;
            peer.close();
            guard.request_stop();
        };
        auto trigger = [&]() -> capy::task<>
        {
            parked = !done;
            if(parked)
                raise_error_condition(peer);
            co_return;
        };
        bool expired = false;
        capy::run_async(ioc.get_executor())(waiter());
        capy::run_async(ioc.get_executor())(trigger());
        capy::run_async(ioc.get_executor(), guard.get_token())(
            stop_guard(ioc, expired));
        ioc.run();
        BOOST_TEST(!expired);
        if(!parked)
        {
            skip_unfillable("testErrorEventOnParkedWaitWrite");
            return;
        }
        BOOST_TEST(probe_fired);
        BOOST_TEST(wec == std::errc::bad_file_descriptor);
        BOOST_TEST(c.is_open());
    }

    /* Poll `target` until the except set is raised, and -- where the
       parked operation is waiting on the send window -- until it is
       writable in the same answer.

       The dispatch arms need one round to report writability and the
       except set together. Probing for both is the only way the test
       can tell that premise from the two halves holding at different
       moments, which is what a kernel that reopens the window a beat
       late gives it. Bounded well inside the stop_guard's two seconds,
       so a kernel that never shows the pair fails the test rather than
       the guard.

       @param target The descriptor the parked operation is on.
       @param and_writable Whether writability is part of the premise.

       @return True once every bit the caller needs holds at once.
    */
    static bool await_condition(tcp_socket& target, bool and_writable)
    {
        int const fd = static_cast<int>(target.native_handle());
        for(int i = 0; i < 200; ++i)
        {
            fd_set w, ex;
            FD_ZERO(&w);
            FD_ZERO(&ex);
            FD_SET(fd, &w);
            FD_SET(fd, &ex);
            timeval tv{0, 1000};
            if(::select(fd + 1, nullptr, and_writable ? &w : nullptr,
                    &ex, &tv) > 0 &&
                FD_ISSET(fd, &ex) &&
                (!and_writable || FD_ISSET(fd, &w)))
            {
                return true;
            }
        }
        return false;
    }

    /* Raise an except condition on `target` from its peer.

       Out-of-band data is the one condition select reports in the
       except set, and it stays raised until the byte is consumed --
       which is what lets a write-direction readiness bit arrive in the
       same round. The order is load-bearing twice over. A condition
       raised before the operation parks is dispatched on its own, and
       the operation then leaves through the tail block for an error
       with no readiness bit rather than through the write arm; and a
       send is not a delivery, so the turn goes back to the run loop
       only once the condition is observable.

       @return True once the except set is raised on `target`.
    */
    static bool raise_urgent_byte(tcp_socket& peer, tcp_socket& target)
    {
        char oob = '!';
        BOOST_TEST_EQ(::send(peer.native_handle(), &oob, 1, MSG_OOB), 1);
        return await_condition(target, false);
    }

    /* Raise the condition and reopen the window a parked write is
       waiting on.

       Draining comes before the probe, not after: the window has to be
       open for the probe to find it and the except set in one answer,
       and one answer is what the dispatch arm needs.

       @return True once writability and the except set hold together.
    */
    static bool
    raise_writable_error_condition(tcp_socket& peer, tcp_socket& target)
    {
        char oob = '!';
        BOOST_TEST_EQ(::send(peer.native_handle(), &oob, 1, MSG_OOB), 1);
        drain_receive_buffer(peer);
        return await_condition(target, true);
    }

    // Report a kernel that will not show us the condition the two
    // write-direction arms need.
    static void skip_unraisable(char const* what)
    {
        std::fprintf(stderr,
            "fault harness: an urgent byte did not raise the except set "
            "on this kernel; skipping %s\n", what);
    }

    /* Report a premise that held for the probe and not for the reactor.

       Only the round before the reactor's can be probed from a
       coroutine, so a kernel that drops one of the two bits in between
       leaves the operation resolving normally -- the arm never ran, and
       there is nothing here to assert about.
    */
    static void skip_unpaired(char const* what)
    {
        std::fprintf(stderr,
            "fault harness: the reactor's round did not carry both "
            "writability and the except set; skipping %s\n", what);
    }

    /* The write-direction error arms, reached without a socket error.

       Both arms need the same round to report writability and an error
       condition on one descriptor, and a reset does not do that on the
       BSD family -- it surfaces as plain writability, so the operation
       re-runs its I/O and reports the real error instead. An urgent
       byte does: it raises select's except set on a descriptor that is
       writable in its own right.

       The SO_ERROR probe is left unfaulted here on purpose. An
       out-of-band condition leaves no socket error behind, so the probe
       reads back zero and the dispatch substitutes EIO
       (reactor_descriptor_state::invoke_deferred_io) -- which is also
       what the operation reports, on a socket that is in no way
       broken.
    */
    void testErrorEventOnWritableWrite()
    {
        if constexpr(is_select)
        {
            io_context ioc(Backend);
            auto [c, peer] = test::make_socket_pair(ioc);
            char buf[8] = "1234567";
            std::stop_source guard;
            std::error_code wec;
            bool open_after = false, spec_fired = false, raised = false;
            auto writer = [&]() -> capy::task<>
            {
                // Refusing the speculative write is what parks the
                // operation. Backpressure would do it too, but how much
                // a socket takes before it refuses is the kernel's
                // business and a window that closed can reopen before
                // the operation reaches it.
                fault_scope spec(spec_write, EAGAIN);
                auto [ec, n] = co_await c.write_some(
                    capy::const_buffer(buf, 7));
                std::ignore = n;
                wec = ec;
                spec_fired = spec.fired();
                // Nothing broke: the condition the dispatch reported
                // was one byte of urgent data.
                open_after = c.is_open();
                // The byte is never consumed, so the except set stays
                // raised; the descriptor that raised it goes before the
                // run loop is asked for another pass.
                c.close();
                guard.request_stop();
            };
            auto trigger = [&]() -> capy::task<>
            {
                raised = raise_urgent_byte(peer, c);
                co_return;
            };
            bool expired = false;
            capy::run_async(ioc.get_executor())(writer());
            capy::run_async(ioc.get_executor())(trigger());
            capy::run_async(ioc.get_executor(), guard.get_token())(
                stop_guard(ioc, expired));
            ioc.run();
            BOOST_TEST(!expired);
            BOOST_TEST(spec_fired);
            if(!raised)
            {
                skip_unraisable("testErrorEventOnWritableWrite");
                return;
            }
            BOOST_TEST(wec == std::errc::io_error);
            BOOST_TEST(open_after);
        }
    }

    void testErrorEventOnWritableWaitWrite()
    {
        if constexpr(is_select)
        {
            io_context ioc(Backend);
            auto [c, peer] = make_backpressured_pair(ioc);
            if(!fill_send_buffer(c))
            {
                skip_unfillable("testErrorEventOnWritableWaitWrite");
                return;
            }
            std::stop_source guard;
            std::error_code wec;
            bool done = false, parked = false, open_after = false;
            bool raised = false;
            auto waiter = [&]() -> capy::task<>
            {
                auto [ec] = co_await c.wait(wait_type::write);
                wec = ec;
                done = true;
                open_after = c.is_open();
                c.close();
                guard.request_stop();
            };
            auto trigger = [&]() -> capy::task<>
            {
                parked = !done;
                if(parked)
                    raised = raise_writable_error_condition(peer, c);
                co_return;
            };
            bool expired = false;
            capy::run_async(ioc.get_executor())(waiter());
            capy::run_async(ioc.get_executor())(trigger());
            capy::run_async(ioc.get_executor(), guard.get_token())(
                stop_guard(ioc, expired));
            ioc.run();
            BOOST_TEST(!expired);
            if(!parked)
            {
                skip_unfillable("testErrorEventOnWritableWaitWrite");
                return;
            }
            if(!raised)
            {
                skip_unraisable("testErrorEventOnWritableWaitWrite");
                return;
            }
            if(!wec)
            {
                skip_unpaired("testErrorEventOnWritableWaitWrite");
                return;
            }
            BOOST_TEST(wec == std::errc::io_error);
            BOOST_TEST(open_after);
        }
    }

    /* The address family a socket was created with is read back from
       the kernel, not remembered, and a v4 destination on a v6 socket
       is the one case where the answer changes the sockaddr handed to
       connect(). A failed probe reports AF_UNSPEC, which yields the
       v4 shape and an address the v6 socket cannot use.
    */
    void testSocketFamilyProbeFails()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc, loopback());
        tcp_socket s(ioc);
        if(s.open(tcp::v6()))
        {
            std::fprintf(stderr,
                "fault harness: no IPv6 socket on this host; skipping "
                "testSocketFamilyProbeFails\n");
            return;
        }
        std::error_code cec;
        unsigned probes = 0;
        auto body = [&]() -> capy::task<>
        {
            fault_scope f(sys::getsockname, EBADF);
            auto [ec] = co_await s.connect(acc.local_endpoint());
            cec = ec;
            probes = f.count();
            BOOST_TEST(f.fired());
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST_EQ(probes, 1u);
        // Which code a kernel picks for an address in the wrong family
        // is its own choice: Linux rejects the sockaddr as too short
        // for AF_INET6, the BSDs reject the family itself.
#if defined(__linux__)
        BOOST_TEST(cec == std::errc::invalid_argument);
#else
        BOOST_TEST(cec == std::errc::address_family_not_supported);
#endif
        BOOST_TEST(s.is_open());
    }

    void testErrorEventSoError()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc, loopback());
        std::error_code rec;
        auto body = [&]() -> capy::task<>
        {
            tcp_socket c(ioc), s(ioc);
            {
                auto [ec] = co_await c.connect(acc.local_endpoint());
                BOOST_TEST(!ec);
            }
            {
                auto [ec] = co_await acc.accept(s);
                BOOST_TEST(!ec);
            }
            if constexpr(is_select)
            {
                // select() reports an exceptional condition only for
                // out-of-band data; a RST shows up as plain readability
                // and never reaches the SO_ERROR probe. One OOB byte is
                // the portable way to raise the except set.
                char oob = '!';
                BOOST_TEST_EQ(
                    ::send(s.native_handle(), &oob, 1, MSG_OOB), 1);
            }
            else
            {
                // SO_LINGER 0 makes close send a RST, which surfaces as
                // an error event on the parked reader.
                s.set_option(socket_option::linger(true, 0));
            }
            // Nothing else on this path reads a socket option, so the
            // armed getsockopt is the reactor's SO_ERROR probe.
            fault_scope f1(sys::recv, EAGAIN);
            fault_scope f2(sys::getsockopt, EBADF);
            // The frame only holds a pointer to the closure, so the
            // name has to outlive the read below, not just the spawn.
            [[maybe_unused]] auto closer = [&]() -> capy::task<>
            {
                s.close();
                co_return;
            };
            if constexpr(!is_select)
            {
                capy::run_async(ioc.get_executor())(closer());
            }
            char buf[4];
            auto [ec, n] = co_await c.read_some(capy::mutable_buffer(buf, 4));
            std::ignore = n;
            rec = ec;
            BOOST_TEST(f1.fired());
            BOOST_TEST(f2.fired());
            if constexpr(is_select)
            {
                // The OOB byte keeps the except set raised, so the
                // socket that raised it has to go before the run loop
                // is asked for another pass.
                s.close();
            }
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST(rec == std::errc::bad_file_descriptor);
    }

    void testDatagramFails()
    {
        io_context ioc(Backend);
        udp_socket a(ioc), b(ioc);
        BOOST_TEST(!a.open(udp::v4()));
        BOOST_TEST(!b.open(udp::v4()));
        BOOST_TEST(!a.bind(loopback()));
        BOOST_TEST(!b.bind(loopback()));
        {
            fault_scope f(sys::connect, ENETUNREACH);
            std::error_code cec;
            auto conn = [&]() -> capy::task<>
            {
                auto [ec] = co_await a.connect(b.local_endpoint());
                cec = ec;
            };
            capy::run_async(ioc.get_executor())(conn());
            ioc.run();
            ioc.restart();
            BOOST_TEST(f.fired());
            BOOST_TEST(cec == std::errc::network_unreachable);
        }
        {
            fault_scope f(sys::shutdown, ENOTCONN);
            BOOST_TEST(a.shutdown(shutdown_both) == std::errc::not_connected);
            BOOST_TEST(f.fired());
        }
        char buf[8] = "1234567";
        std::error_code stec, rfec, sec, rec, dstec, drfec, dsfec, drrec;
        auto body = [&]() -> capy::task<>
        {
            {
                fault_scope f(sys::sendmsg, EPERM);
                auto [ec, n] = co_await a.send_to(
                    capy::const_buffer(buf, 7), b.local_endpoint());
                std::ignore = n;
                stec = ec;
                BOOST_TEST(f.fired());
            }
            {
                auto [ec, n] = co_await a.send_to(
                    capy::const_buffer(buf, 7), b.local_endpoint());
                std::ignore = n;
                BOOST_TEST(!ec);
            }
            {
                fault_scope f(sys::recvmsg, EIO);
                endpoint from;
                auto [ec, n] = co_await b.recv_from(
                    capy::mutable_buffer(buf, 7), from);
                std::ignore = n;
                rfec = ec;
                BOOST_TEST(f.fired());
            }
            {
                endpoint from;
                auto [ec, n] = co_await b.recv_from(
                    capy::mutable_buffer(buf, 7), from);
                std::ignore = n;
                BOOST_TEST(!ec);
            }
            // Deferred send_to. A faulted EAGAIN cannot make a UDP
            // socket unwritable, so no later edge would ever arrive to
            // retry a parked op on an edge-triggered backend. Use a
            // socket registered this instant and hop the run loop once,
            // which dispatches the registration's writable event and
            // latches desc_state.write_ready; register_op then runs the
            // parked op's perform_io() inline instead of waiting for an
            // edge. select is level-triggered and re-reports the fd as
            // soon as the op opts into the write set, so the hop is
            // merely harmless there.
            {
                udp_socket d(ioc);
                BOOST_TEST(!d.open(udp::v4()));
                BOOST_TEST(!d.bind(loopback()));
                std::ignore = co_await corosio::delay(
                    std::chrono::milliseconds(1));
                fault_scope f1(sys::sendmsg, EAGAIN);
                fault_scope f2(sys::sendmsg, EIO, 2);
                auto [ec, n] = co_await d.send_to(
                    capy::const_buffer(buf, 7), b.local_endpoint());
                std::ignore = n;
                dsfec = ec;
                BOOST_TEST(f1.fired());
                BOOST_TEST(f2.fired());
            }
            {
                fault_scope f1(sys::recvmsg, EAGAIN);
                fault_scope f2(sys::recvmsg, EIO, 2);
                auto sender = [&]() -> capy::task<>
                {
                    auto [ec, n] = co_await a.send_to(
                        capy::const_buffer(buf, 4), b.local_endpoint());
                    std::ignore = n;
                    BOOST_TEST(!ec);
                };
                capy::run_async(ioc.get_executor())(sender());
                endpoint from;
                auto [ec, n] = co_await b.recv_from(
                    capy::mutable_buffer(buf, 7), from);
                std::ignore = n;
                drrec = ec;
                BOOST_TEST(f1.fired());
                BOOST_TEST(f2.fired());
            }
            {
                auto [ec] = co_await a.connect(b.local_endpoint());
                BOOST_TEST(!ec);
            }
            {
                fault_scope f(sys::sendmsg, EPERM);
                auto [ec, n] = co_await a.send(capy::const_buffer(buf, 7));
                std::ignore = n;
                sec = ec;
                BOOST_TEST(f.fired());
            }
            {
                auto [ec, n] = co_await a.send(capy::const_buffer(buf, 7));
                std::ignore = n;
                BOOST_TEST(!ec);
            }
            {
                fault_scope f(sys::recvmsg, EIO);
                auto [ec, n] = co_await b.recv(capy::mutable_buffer(buf, 7));
                std::ignore = n;
                rec = ec;
                BOOST_TEST(f.fired());
            }
            {
                auto [ec, n] = co_await b.recv(capy::mutable_buffer(buf, 7));
                std::ignore = n;
                BOOST_TEST(!ec);
            }
            {
                fault_scope f1(sys::recvmsg, EAGAIN);
                fault_scope f2(sys::recvmsg, EIO, 2);
                auto sender = [&]() -> capy::task<>
                {
                    auto [ec, n] = co_await a.send(
                        capy::const_buffer(buf, 4));
                    std::ignore = n;
                    BOOST_TEST(!ec);
                };
                capy::run_async(ioc.get_executor())(sender());
                auto [ec, n] = co_await b.recv(capy::mutable_buffer(buf, 7));
                std::ignore = n;
                drfec = ec;
                BOOST_TEST(f1.fired());
                BOOST_TEST(f2.fired());
            }
            // Deferred connected send; same latching hop as send_to.
            {
                udp_socket d(ioc);
                BOOST_TEST(!d.open(udp::v4()));
                BOOST_TEST(!d.bind(loopback()));
                {
                    auto [ec] = co_await d.connect(b.local_endpoint());
                    BOOST_TEST(!ec);
                }
                std::ignore = co_await corosio::delay(
                    std::chrono::milliseconds(1));
                fault_scope f1(sys::sendmsg, EAGAIN);
                fault_scope f2(sys::sendmsg, EIO, 2);
                auto [ec, n] = co_await d.send(capy::const_buffer(buf, 7));
                std::ignore = n;
                dstec = ec;
                BOOST_TEST(f1.fired());
                BOOST_TEST(f2.fired());
            }
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST(stec == std::errc::operation_not_permitted);
        BOOST_TEST(rfec == std::errc::io_error);
        BOOST_TEST(sec == std::errc::operation_not_permitted);
        BOOST_TEST(rec == std::errc::io_error);
        BOOST_TEST(dsfec == std::errc::io_error);
        BOOST_TEST(drrec == std::errc::io_error);
        BOOST_TEST(drfec == std::errc::io_error);
        BOOST_TEST(dstec == std::errc::io_error);
    }

    void run()
    {
        if(skip_under_valgrind())
            return;
        testAcceptorFails();
        testConnectFails();
        testStreamIoFails();
        testDeferredWriteFails();
        testShutdownAndWaitFails();
        testErrorEventSoError();
        testErrorEventOnParkedWaitRead();
        testErrorEventOnParkedWaitError();
#if defined(__linux__)
        testErrorEventOnParkedWrite();
        testErrorEventOnParkedWaitWrite();
#endif
        testErrorEventOnWritableWrite();
        testErrorEventOnWritableWaitWrite();
        testSocketFamilyProbeFails();
        testDatagramFails();
    }
};

} // boost::corosio::test::fault

#endif
