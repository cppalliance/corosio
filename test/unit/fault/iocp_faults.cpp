//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "fault.hpp"
#include "fault_test_utils.hpp"
#include "context.hpp"
#include "test_suite.hpp"
#include "test_utils.hpp"

#include <boost/corosio/delay.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/corosio/test/temp_path.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <coroutine>
#include <limits>
#include <thread>
#include <optional>
#include <system_error>
#include <tuple>

#if BOOST_COROSIO_HAS_IOCP

// Some Windows SDKs still ship winsock2.h without the AF_UNIX name the
// library's own sources spell out the same way.
#ifndef AF_UNIX
#define AF_UNIX 1
#endif

namespace boost::corosio::test::fault {

namespace {

endpoint loopback()
{
    return endpoint(ipv4_address::loopback(), 0);
}

// Reach win_scheduler::post(continuation&) without a heap allocation,
// which is the overload whose PostQueuedCompletionStatus failure has a
// fallback of its own.
struct post_awaitable
{
    capy::continuation* cont;

    bool await_ready() const noexcept { return false; }

    void await_suspend(
        std::coroutine_handle<> h, capy::io_env const* env) noexcept
    {
        cont->h = h;
        env->executor.post(*cont);
    }

    void await_resume() const noexcept {}
};

// One entry point make_wakeup_pair calls, the code to fail it with,
// and which of its calls to that entry point to hit.
struct wakeup_arm
{
    sys which;
    int err;
    unsigned nth = 1;
};

// An operation the wait reactor can no longer complete parks forever,
// and a run loop that never returns reads as a job timeout on CI
// rather than as a failure. This turns that into an assertion.
capy::task<> stop_guard(io_context& ioc, bool& expired)
{
    std::ignore = co_await corosio::delay(std::chrono::seconds(2));
    expired = true;
    ioc.stop();
}

} // namespace

/* Faults on the IOCP backend itself: the scheduler, its completion
   dequeue, the socket services and the auxiliary wait reactor. The
   file, resolver, host_name and signal paths live in win_faults.cpp.
*/
struct iocp_faults
{
    void testSchedulerConstructFails()
    {
        // Winsock is started once per process and released when the
        // last service goes, so this only fires while no io_context is
        // alive. The resolver service that starts it is built inside
        // the scheduler's constructor, after the completion port: the
        // port is the scheduler's to release on the way out.
        //
        // A lost port is exactly one handle per attempt, so the run is
        // twice as long as the growth it allows: sixteen attempts have
        // to stay under the eight handles of ambient drift the default
        // shape tolerates.
        expect_no_handle_leak([]{
            fault_scope f(sys::WSAStartup, WSAEAFNOSUPPORT);
            expect_system_error([]{ io_context ioc(iocp); },
                std::errc::address_family_not_supported);
            BOOST_TEST(f.fired());
        }, 16, 8);
        {
            // The scheduler's own port: CreateIoCompletionPort with
            // INVALID_HANDLE_VALUE.
            fault_scope f(sys::CreateIoCompletionPort,
                ERROR_INVALID_PARAMETER);
            expect_system_error([]{ io_context ioc(iocp); },
                win_err(ERROR_INVALID_PARAMETER));
            BOOST_TEST(f.fired());
        }
    }

    void testTimerCreationFails()
    {
        // A null waitable timer is never reported: start() returns
        // without a thread and update_timeout() does nothing, so
        // timers simply never fire.
        fault_scope f(sys::CreateWaitableTimerW, ERROR_NOT_ENOUGH_MEMORY);
        io_context ioc(iocp);
        BOOST_TEST(f.fired());

        bool fired_timer = false;
        auto body = [&]() -> capy::task<>
        {
            std::ignore = co_await corosio::delay(
                std::chrono::milliseconds(1));
            fired_timer = true;
        };
        capy::run_async(ioc.get_executor())(body());
        // Nothing else can end the loop, so the run is bounded by the
        // stop rather than by the timer.
        ioc.stop();
        ioc.run();
        // A smoke check rather than a proof: the stop latches before
        // run() begins, so the loop would have returned early even with
        // a working timer. What the arm proves is that construction
        // swallowed the failure; this only shows nothing fired anyway.
        BOOST_TEST(!fired_timer);
    }

    void testTimerThreadWaitFails()
    {
        // The wait is the first thing the timer thread does, but the
        // thread starts asynchronously: destroying the context right
        // away can set the shutdown flag before the loop is entered
        // and the wait never happens.
        // So the context is held until the arm reports, bounded so a
        // wait that never comes fails rather than hangs.
        //
        // WaitForSingleObject is not ours alone: the CRT and the
        // standard library reach it through the same import thunk --
        // std::thread::join at the end of this scope certainly does,
        // and a first-use initialization anywhere might. Any of those
        // would spend the process-wide arm on the caller and leave
        // this test passing without the timer thread ever having been
        // faulted. A thread-local arm shadows the process-wide one for
        // the thread that holds it, and an arm whose nth is out of
        // reach never fires, so `shield` turns every wait on this
        // thread into a plain forward and leaves the process-wide arm
        // for the only other thread in the process.
        fault_scope shield(sys::WaitForSingleObject, ERROR_INVALID_HANDLE,
            (std::numeric_limits<unsigned>::max)());
        fault_scope f(sys::WaitForSingleObject, ERROR_INVALID_HANDLE, 1,
            any_thread);
        {
            io_context ioc(iocp);
            for(int i = 0; i < 2000 && !f.fired(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        BOOST_TEST(f.fired());
        BOOST_TEST(!shield.fired());
    }

    void testStopPostFails()
    {
        io_context ioc(iocp);
        // The shutdown packet is the only thing that wakes a blocked
        // run(), so a failed post is fatal rather than reported.
        fault_scope f(sys::PostQueuedCompletionStatus,
            ERROR_NO_SYSTEM_RESOURCES);
        expect_system_error([&]{ ioc.stop(); },
            win_err(ERROR_NO_SYSTEM_RESOURCES));
        BOOST_TEST(f.fired());
    }

    void testPostFallbackRuns()
    {
        io_context ioc(iocp);
        capy::continuation cont{};
        bool ran = false;
        bool fired = false;
        auto body = [&]() -> capy::task<>
        {
            {
                // A failed post falls back to the allocating handle
                // path, so the work still runs.
                fault_scope f(sys::PostQueuedCompletionStatus,
                    ERROR_NO_SYSTEM_RESOURCES);
                co_await post_awaitable{&cont};
                fired = f.fired();
            }
            ran = true;
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST(fired);
        BOOST_TEST(ran);
    }

    void testRunLoopDequeueFails()
    {
        io_context ioc(iocp);
        // A dequeue that reports failure with no OVERLAPPED is not a
        // timeout, so the run loop throws.
        fault_scope f(sys::GetQueuedCompletionStatus, ERROR_INVALID_HANDLE);
        auto body = [&]() -> capy::task<>
        {
            std::ignore = co_await corosio::delay(
                std::chrono::milliseconds(1));
        };
        capy::run_async(ioc.get_executor())(body());
        expect_system_error([&]{ ioc.run(); },
            win_err(ERROR_INVALID_HANDLE));
        BOOST_TEST(f.fired());
    }

    void testTcpOpenFails()
    {
        io_context ioc(iocp);
        {
            tcp_socket s(ioc);
            fault_scope f(sys::WSASocketW, WSAEAFNOSUPPORT);
            auto ec = s.open(tcp::v4());
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::address_family_not_supported);
            BOOST_TEST(!s.is_open());
        }
        // The socket exists when the association fails, so the
        // failure path owns closing it.
        expect_no_handle_leak([&]{
            tcp_socket s(ioc);
            fault_scope f(sys::CreateIoCompletionPort,
                ERROR_INVALID_PARAMETER);
            auto ec = s.open(tcp::v4());
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == win_err(ERROR_INVALID_PARAMETER));
            BOOST_TEST(!s.is_open());
        });
    }

    void testTcpAssignFails()
    {
        io_context ioc(iocp);
        auto h = make_native_socket(AF_INET, SOCK_STREAM);
        make_native_adoptable(h);
        expect_no_handle_leak([&]{
            {
                // SO_PROTOCOL_INFOW is how adoption learns the
                // family and type.
                tcp_socket s(ioc);
                fault_scope f(sys::getsockopt, WSAENOTSOCK);
                auto ec = s.assign(h);
                BOOST_TEST(f.fired());
                BOOST_TEST(ec == std::errc::not_a_socket);
                BOOST_TEST(!s.is_open());
            }
            {
                tcp_socket s(ioc);
                fault_scope f(sys::CreateIoCompletionPort,
                    ERROR_INVALID_PARAMETER);
                auto ec = s.assign(h);
                BOOST_TEST(f.fired());
                BOOST_TEST(ec == win_err(ERROR_INVALID_PARAMETER));
                BOOST_TEST(!s.is_open());
            }
        });
        // A rejected adoption leaves the socket with the caller.
        BOOST_TEST(native_socket_valid(h));
        close_native_socket(h);
    }

    void testTcpBindFails()
    {
        io_context ioc(iocp);
        tcp_socket s(ioc);
        BOOST_TEST(!s.open(tcp::v4()));
        fault_scope f(sys::bind, WSAEADDRINUSE);
        auto ec = s.bind(loopback());
        BOOST_TEST(f.fired());
        BOOST_TEST(ec == std::errc::address_in_use);
        BOOST_TEST(s.is_open());
    }

    void testTcpOptionsFail()
    {
        io_context ioc(iocp);
        tcp_socket s(ioc);
        BOOST_TEST(!s.open(tcp::v4()));
        {
            fault_scope f(sys::setsockopt, WSAENOTSOCK);
            expect_system_error(
                [&]{ s.set_option(socket_option::reuse_address(true)); },
                std::errc::not_a_socket);
            BOOST_TEST(f.fired());
            BOOST_TEST(s.is_open());
        }
        {
            fault_scope f(sys::getsockopt, WSAENOTSOCK);
            expect_system_error(
                [&]{
                    std::ignore = s.get_option<socket_option::reuse_address>();
                },
                std::errc::not_a_socket);
            BOOST_TEST(f.fired());
            BOOST_TEST(s.is_open());
        }
        {
            fault_scope f(sys::shutdown, WSAENOTSOCK);
            BOOST_TEST(s.shutdown(tcp_socket::shutdown_both) ==
                std::errc::not_a_socket);
            BOOST_TEST(f.fired());
        }
    }

    void testTcpReleaseIgnoresDissociate()
    {
        if(!hook_is_live(sys::NtSetInformationFile))
        {
            skip_dead_hook("NtSetInformationFile");
            return;
        }
        io_context ioc(iocp);
        tcp_socket s(ioc);
        BOOST_TEST(!s.open(tcp::v4()));
        // Severing the port association is best effort: the caller
        // gets a working socket either way.
        fault_scope f(sys::NtSetInformationFile, ERROR_INVALID_PARAMETER);
        auto h = s.release();
        BOOST_TEST(f.fired());
        BOOST_TEST(!s.is_open());
        BOOST_TEST(native_socket_valid(h));
        close_native_socket(h);
    }

    void testTcpExtensionPointerMissing()
    {
        // load_extension_functions runs once, from the tcp service's
        // constructor, so the arm has to precede the io_context.
        fault_scope f(sys::WSAIoctl, WSAEOPNOTSUPP);
        io_context ioc(iocp);
        BOOST_TEST(f.fired());

        tcp_acceptor acc(ioc, loopback());
        auto port = acc.local_endpoint().port();
        tcp_socket s(ioc);
        BOOST_TEST(!s.open(tcp::v4()));
        std::error_code cec;
        auto body = [&]() -> capy::task<>
        {
            auto [ec] = co_await s.connect(
                endpoint(ipv4_address::loopback(), port));
            cec = ec;
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST(cec == std::errc::operation_not_supported);
    }

    void testTcpConnectFails()
    {
        io_context ioc(iocp);
        tcp_acceptor acc(ioc, loopback());
        auto const ep = endpoint(
            ipv4_address::loopback(), acc.local_endpoint().port());
        std::error_code bec, sec, cec;
        bool expired = false;
        auto body = [&]() -> capy::task<>
        {
            {
                // ConnectEx needs a bound socket, so an unbound one
                // is bound to the wildcard first.
                tcp_socket s(ioc);
                BOOST_TEST(!s.open(tcp::v4()));
                fault_scope f(sys::bind, WSAEADDRNOTAVAIL);
                auto [ec] = co_await s.connect(ep);
                bec = ec;
                BOOST_TEST(f.fired());
            }
            if(hook_is_live(sys::ConnectEx))
            {
                tcp_socket s(ioc);
                BOOST_TEST(!s.open(tcp::v4()));
                fault_scope f(sys::ConnectEx, WSAECONNREFUSED);
                auto [ec] = co_await s.connect(ep);
                sec = ec;
                BOOST_TEST(f.fired());
            }
            else
            {
                skip_dead_hook("ConnectEx");
            }
            {
                // The kernel result of a queued connect only exists
                // on the completion.
                tcp_socket s(ioc);
                BOOST_TEST(!s.open(tcp::v4()));
                completion_fault_scope q(ERROR_CONNECTION_REFUSED);
                auto [ec] = co_await s.connect(ep);
                cec = ec;
                BOOST_TEST(q.fired());
            }
            ioc.stop();
        };
        capy::run_async(ioc.get_executor())(body());
        capy::run_async(ioc.get_executor())(stop_guard(ioc, expired));
        ioc.run();
        BOOST_TEST(!expired);
        BOOST_TEST(bec == std::errc::address_not_available);
        if(hook_is_live(sys::ConnectEx))
            BOOST_TEST(sec == std::errc::connection_refused);
        BOOST_TEST(cec == std::errc::connection_refused);
    }

    void testTcpReadWriteFails()
    {
        io_context ioc(iocp);
        auto pair = make_socket_pair(ioc);
        auto& a = pair.first;
        auto& b = pair.second;
        char buf[8] = {};
        char out[4] = "abc";
        std::error_code rec, wec, rcec, wcec, eec, wtec;
        std::size_t en = 99;
        bool expired = false;
        auto body = [&]() -> capy::task<>
        {
            {
                fault_scope f(sys::WSARecv, WSAENOTSOCK);
                auto [ec, n] = co_await a.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                std::ignore = n;
                rec = ec;
                BOOST_TEST(f.fired());
            }
            {
                fault_scope f(sys::WSASend, WSAENOTSOCK);
                auto [ec, n] = co_await a.write_some(
                    capy::const_buffer(out, 3));
                std::ignore = n;
                wec = ec;
                BOOST_TEST(f.fired());
            }
            {
                // A wait for readability is a zero-byte WSARecv.
                fault_scope f(sys::WSARecv, WSAENOTSOCK);
                auto [ec] = co_await a.wait(wait_type::read);
                wtec = ec;
                BOOST_TEST(f.fired());
            }
            {
                auto [ec, n] = co_await b.write_some(
                    capy::const_buffer(out, 3));
                std::ignore = n;
                BOOST_TEST(!ec);
            }
            {
                // A remote reset reaches a pending read as
                // ERROR_NETNAME_DELETED, which off the accept path
                // means connection_reset.
                completion_fault_scope q(ERROR_NETNAME_DELETED);
                auto [ec, n] = co_await a.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                std::ignore = n;
                rcec = ec;
                BOOST_TEST(q.fired());
            }
            {
                completion_fault_scope q(ERROR_NETNAME_DELETED);
                auto [ec, n] = co_await a.write_some(
                    capy::const_buffer(out, 3));
                std::ignore = n;
                wcec = ec;
                BOOST_TEST(q.fired());
            }
            {
                auto [ec, n] = co_await b.write_some(
                    capy::const_buffer(out, 3));
                std::ignore = n;
                BOOST_TEST(!ec);
            }
            {
                // A receive shortened to nothing completes with zero
                // bytes even with data waiting, which the stream
                // contract reads as end of file.
                auto f = fault_scope::returning(sys::WSARecv, 0);
                auto [ec, n] = co_await a.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                eec = ec;
                en = n;
                BOOST_TEST(f.fired());
            }
            a.cancel();
            b.cancel();
            ioc.stop();
        };
        capy::run_async(ioc.get_executor())(body());
        capy::run_async(ioc.get_executor())(stop_guard(ioc, expired));
        ioc.run();
        BOOST_TEST(!expired);
        BOOST_TEST(rec == std::errc::not_a_socket);
        BOOST_TEST(wec == std::errc::not_a_socket);
        BOOST_TEST(wtec == std::errc::not_a_socket);
        BOOST_TEST(rcec == std::errc::connection_reset);
        BOOST_TEST(wcec == std::errc::connection_reset);
        BOOST_TEST(eec == capy::error::eof);
        BOOST_TEST_EQ(en, 0u);
    }

    void testAcceptorOpenFails()
    {
        io_context ioc(iocp);
        {
            tcp_acceptor acc(ioc);
            fault_scope f(sys::WSASocketW, WSAEAFNOSUPPORT);
            auto ec = acc.open();
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::address_family_not_supported);
            BOOST_TEST(!acc.is_open());
        }
        expect_no_handle_leak([&]{
            tcp_acceptor acc(ioc);
            fault_scope f(sys::CreateIoCompletionPort,
                ERROR_INVALID_PARAMETER);
            auto ec = acc.open();
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == win_err(ERROR_INVALID_PARAMETER));
            BOOST_TEST(!acc.is_open());
        });
        {
            tcp_acceptor acc(ioc);
            BOOST_TEST(!acc.open());
            fault_scope f(sys::bind, WSAEADDRINUSE);
            BOOST_TEST(acc.bind(loopback()) == std::errc::address_in_use);
            BOOST_TEST(f.fired());
        }
        {
            tcp_acceptor acc(ioc);
            BOOST_TEST(!acc.open());
            BOOST_TEST(!acc.bind(loopback()));
            fault_scope f(sys::listen, WSAEOPNOTSUPP);
            BOOST_TEST(acc.listen() == std::errc::operation_not_supported);
            BOOST_TEST(f.fired());
        }
        {
            // The convenience constructor reports the same codes by
            // throwing.
            fault_scope f(sys::listen, WSAEOPNOTSUPP);
            expect_system_error([&]{ tcp_acceptor acc(ioc, loopback()); },
                std::errc::operation_not_supported);
            BOOST_TEST(f.fired());
        }
    }

    void testAcceptFails()
    {
        io_context ioc(iocp);
        tcp_acceptor acc(ioc, loopback());
        auto const ep = endpoint(
            ipv4_address::loopback(), acc.local_endpoint().port());
        std::error_code sockec, portec, syncec, compec, waitec;
        bool expired = false;
        auto body = [&]() -> capy::task<>
        {
            tcp_socket server(ioc);
            {
                // Writability carries no meaning for a listening
                // socket and reaches no syscall.
                auto [ec] = co_await acc.wait(wait_type::write);
                waitec = ec;
            }
            {
                tcp_socket client(ioc);
                auto [cec] = co_await client.connect(ep);
                BOOST_TEST(!cec);
                fault_scope f(sys::WSASocketW, WSAEAFNOSUPPORT);
                auto [ec] = co_await acc.accept(server);
                sockec = ec;
                BOOST_TEST(f.fired());
                BOOST_TEST(!server.is_open());
                client.cancel();
                client.close();
            }
            {
                tcp_socket client(ioc);
                auto [cec] = co_await client.connect(ep);
                BOOST_TEST(!cec);
                // No handle-count assertion here: the client socket
                // this accept needs is created inside the same window,
                // so the count carries more than the accept's own
                // bookkeeping.
                fault_scope f(sys::CreateIoCompletionPort,
                    ERROR_INVALID_PARAMETER);
                auto [ec] = co_await acc.accept(server);
                portec = ec;
                BOOST_TEST(f.fired());
                BOOST_TEST(!server.is_open());
                client.cancel();
                client.close();
            }
            if(hook_is_live(sys::AcceptEx))
            {
                tcp_socket client(ioc);
                auto [cec] = co_await client.connect(ep);
                BOOST_TEST(!cec);
                fault_scope f(sys::AcceptEx, WSAENOTSOCK);
                auto [ec] = co_await acc.accept(server);
                syncec = ec;
                BOOST_TEST(f.fired());
                BOOST_TEST(!server.is_open());
                client.cancel();
                client.close();
            }
            else
            {
                skip_dead_hook("AcceptEx");
            }
            {
                tcp_socket client(ioc);
                auto [cec] = co_await client.connect(ep);
                BOOST_TEST(!cec);
                // On the accept path ERROR_NETNAME_DELETED means the
                // half-open connection died, not a reset stream.
                completion_fault_scope q(ERROR_NETNAME_DELETED);
                auto [ec] = co_await acc.accept(server);
                compec = ec;
                BOOST_TEST(q.fired());
                BOOST_TEST(!server.is_open());
                client.cancel();
                client.close();
            }
            ioc.stop();
        };
        capy::run_async(ioc.get_executor())(body());
        capy::run_async(ioc.get_executor())(stop_guard(ioc, expired));
        ioc.run();
        BOOST_TEST(!expired);
        BOOST_TEST(waitec == std::errc::operation_not_supported);
        BOOST_TEST(sockec == std::errc::address_family_not_supported);
        BOOST_TEST(portec == win_err(ERROR_INVALID_PARAMETER));
        if(hook_is_live(sys::AcceptEx))
            BOOST_TEST(syncec == std::errc::not_a_socket);
        BOOST_TEST(compec == std::errc::connection_aborted);
    }

    void testUdpSetupFails()
    {
        io_context ioc(iocp);
        {
            udp_socket u(ioc);
            fault_scope f(sys::WSASocketW, WSAEAFNOSUPPORT);
            auto ec = u.open(udp::v4());
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::address_family_not_supported);
            BOOST_TEST(!u.is_open());
        }
        expect_no_handle_leak([&]{
            udp_socket u(ioc);
            fault_scope f(sys::CreateIoCompletionPort,
                ERROR_INVALID_PARAMETER);
            auto ec = u.open(udp::v4());
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == win_err(ERROR_INVALID_PARAMETER));
            BOOST_TEST(!u.is_open());
        });
        {
            udp_socket u(ioc);
            BOOST_TEST(!u.open(udp::v4()));
            fault_scope f(sys::bind, WSAEADDRINUSE);
            BOOST_TEST(u.bind(loopback()) == std::errc::address_in_use);
            BOOST_TEST(f.fired());
            BOOST_TEST(u.is_open());
        }
        {
            auto h = make_native_socket(AF_INET, SOCK_DGRAM);
            make_native_adoptable(h);
            expect_no_handle_leak([&]{
                {
                    udp_socket u(ioc);
                    fault_scope f(sys::getsockopt, WSAENOTSOCK);
                    BOOST_TEST(u.assign(h) == std::errc::not_a_socket);
                    BOOST_TEST(f.fired());
                    BOOST_TEST(!u.is_open());
                }
                {
                    udp_socket u(ioc);
                    fault_scope f(sys::CreateIoCompletionPort,
                        ERROR_INVALID_PARAMETER);
                    BOOST_TEST(
                        u.assign(h) == win_err(ERROR_INVALID_PARAMETER));
                    BOOST_TEST(f.fired());
                    BOOST_TEST(!u.is_open());
                }
            });
            BOOST_TEST(native_socket_valid(h));
            close_native_socket(h);
        }
        {
            udp_socket u(ioc);
            BOOST_TEST(!u.open(udp::v4()));
            {
                fault_scope f(sys::setsockopt, WSAENOTSOCK);
                expect_system_error(
                    [&]{ u.set_option(socket_option::reuse_address(true)); },
                    std::errc::not_a_socket);
                BOOST_TEST(f.fired());
            }
            {
                fault_scope f(sys::getsockopt, WSAENOTSOCK);
                expect_system_error(
                    [&]{
                        std::ignore =
                            u.get_option<socket_option::reuse_address>();
                    },
                    std::errc::not_a_socket);
                BOOST_TEST(f.fired());
            }
            {
                fault_scope f(sys::shutdown, WSAENOTSOCK);
                BOOST_TEST(u.shutdown(udp_socket::shutdown_both) ==
                    std::errc::not_a_socket);
                BOOST_TEST(f.fired());
            }
        }
    }

    void testUdpIoFails()
    {
        io_context ioc(iocp);
        udp_socket a(ioc), b(ioc);
        BOOST_TEST(!a.open(udp::v4()));
        BOOST_TEST(!a.bind(loopback()));
        BOOST_TEST(!b.open(udp::v4()));
        BOOST_TEST(!b.bind(loopback()));
        auto const a_ep = endpoint(
            ipv4_address::loopback(), a.local_endpoint().port());
        auto const b_ep = endpoint(
            ipv4_address::loopback(), b.local_endpoint().port());
        char buf[8] = {};
        char out[4] = "abc";
        std::error_code stec, rfec, stcec, rfcec, conec, sec, rec, scec, rcec;
        bool expired = false;
        auto body = [&]() -> capy::task<>
        {
            {
                fault_scope f(sys::WSASendTo, WSAENOTSOCK);
                auto [ec, n] = co_await a.send_to(
                    capy::const_buffer(out, 3), b_ep);
                std::ignore = n;
                stec = ec;
                BOOST_TEST(f.fired());
            }
            {
                endpoint src;
                fault_scope f(sys::WSARecvFrom, WSAENOTSOCK);
                auto [ec, n] = co_await b.recv_from(
                    capy::mutable_buffer(buf, sizeof(buf)), src);
                std::ignore = n;
                rfec = ec;
                BOOST_TEST(f.fired());
            }
            {
                // The kernel result of a queued WSASendTo only exists
                // on the completion, so the unconnected send has its
                // own completion fault.
                completion_fault_scope q(ERROR_NETNAME_DELETED);
                auto [ec, n] = co_await a.send_to(
                    capy::const_buffer(out, 3), b_ep);
                std::ignore = n;
                stcec = ec;
                BOOST_TEST(q.fired());
            }
            {
                // That datagram really went out — only its completion
                // was rewritten — so the receive has a real one to
                // fail in turn.
                endpoint src;
                completion_fault_scope q(ERROR_NETNAME_DELETED);
                auto [ec, n] = co_await b.recv_from(
                    capy::mutable_buffer(buf, sizeof(buf)), src);
                std::ignore = n;
                rfcec = ec;
                BOOST_TEST(q.fired());
            }
            {
                // A datagram connect is synchronous: WSAConnect
                // either names the peer or reports why not.
                fault_scope f(sys::WSAConnect, WSAEAFNOSUPPORT);
                auto [ec] = co_await a.connect(b_ep);
                conec = ec;
                BOOST_TEST(f.fired());
            }
            {
                auto [ec] = co_await a.connect(b_ep);
                BOOST_TEST(!ec);
            }
            {
                auto [ec] = co_await b.connect(a_ep);
                BOOST_TEST(!ec);
            }
            {
                fault_scope f(sys::WSASend, WSAENOTSOCK);
                auto [ec, n] = co_await a.send(capy::const_buffer(out, 3));
                std::ignore = n;
                sec = ec;
                BOOST_TEST(f.fired());
            }
            {
                fault_scope f(sys::WSARecv, WSAENOTSOCK);
                auto [ec, n] = co_await b.recv(
                    capy::mutable_buffer(buf, sizeof(buf)));
                std::ignore = n;
                rec = ec;
                BOOST_TEST(f.fired());
            }
            {
                completion_fault_scope q(ERROR_NETNAME_DELETED);
                auto [ec, n] = co_await a.send(capy::const_buffer(out, 3));
                std::ignore = n;
                scec = ec;
                BOOST_TEST(q.fired());
            }
            {
                // The datagram sent above is already queued, so the
                // receive has a real completion to fail.
                completion_fault_scope q(ERROR_NETNAME_DELETED);
                auto [ec, n] = co_await b.recv(
                    capy::mutable_buffer(buf, sizeof(buf)));
                std::ignore = n;
                rcec = ec;
                BOOST_TEST(q.fired());
            }
            ioc.stop();
        };
        capy::run_async(ioc.get_executor())(body());
        capy::run_async(ioc.get_executor())(stop_guard(ioc, expired));
        ioc.run();
        // A receive parks until its datagram lands; the guard turns a
        // dropped one into a failure instead of a stalled run loop.
        BOOST_TEST(!expired);
        BOOST_TEST(stec == std::errc::not_a_socket);
        BOOST_TEST(rfec == std::errc::not_a_socket);
        BOOST_TEST(stcec == std::errc::connection_reset);
        BOOST_TEST(rfcec == std::errc::connection_reset);
        BOOST_TEST(conec == std::errc::address_family_not_supported);
        BOOST_TEST(sec == std::errc::not_a_socket);
        BOOST_TEST(rec == std::errc::not_a_socket);
        BOOST_TEST(scec == std::errc::connection_reset);
        BOOST_TEST(rcec == std::errc::connection_reset);
    }

    void testWaitReactorSetupFails()
    {
        // The wakeup channel is built as the scheduler is, right after
        // the resolver service starts Winsock, and nothing before it
        // in the construction reaches these entry points -- so the
        // counts below are the pair's own. A reactor that cannot be
        // woken can never report readiness, so the context refuses to
        // construct rather than handing back one whose every wait
        // would park forever. The codes are ones neither make_err nor
        // iocp_make_err rewrites, so what is injected is what the
        // constructor throws.
        static constexpr wakeup_arm arms[] = {
            {sys::socket, WSAEMFILE},
            {sys::bind, WSAEACCES},
            {sys::listen, WSAEINVAL},
            {sys::getsockname, WSAEFAULT},
            // The second socket() in make_wakeup_pair is the peer that
            // connects to the listener.
            {sys::socket, WSAENOBUFS, 2u},
            {sys::connect, WSAENETDOWN},
            {sys::accept, WSAEINPROGRESS},
            {sys::ioctlsocket, WSAENOTCONN},
        };
        // The completion port, the timer wakeup and whichever wakeup
        // sockets were already open are all the failed construction's
        // to release. A leak of one handle per attempt has to separate
        // from the ambient drift, so each arm runs sixteen times
        // against a growth budget of eight.
        for(auto const& arm : arms)
        {
            expect_no_handle_leak([&]{
                fault_scope f(arm.which, arm.err, arm.nth);
                expect_system_error([]{ io_context ioc(iocp); },
                    win_err(arm.err));
                BOOST_TEST(f.fired());
            }, 16, 8);
        }
    }

    // A context that constructs owns a wakeup channel, so the reactor
    // has something to poll and the wait it is handed completes. The
    // polling thread is the half that waits for a reason to exist: one
    // that was already running would be sitting in WSAPoll, so a
    // context that never waits leaves the arm below unspent.
    void testWaitReactorStartsOnFirstWait()
    {
        if(!hook_is_live(sys::WSAPoll))
        {
            skip_dead_hook("WSAPoll");
        }
        else
        {
            // any_thread: a thread that had started would be polling
            // on its own, not on this one. The arm outlives the
            // context so a poll begun during teardown counts too.
            fault_scope f(sys::WSAPoll, WSAENOBUFS, 1u, any_thread);
            {
                io_context quiet(iocp);
                udp_socket s(quiet);
                BOOST_TEST(!s.open(udp::v4()));
                BOOST_TEST(!s.bind(loopback()));
                s.close();
            }
            BOOST_TEST(!f.fired());
        }

        io_context ioc(iocp);
        auto pair = make_socket_pair(ioc);
        auto& s1 = pair.first;
        auto& s2 = pair.second;
        std::error_code wec;
        bool done    = false;
        bool expired = false;
        auto body = [&]() -> capy::task<>
        {
            auto [ec] = co_await s1.wait(wait_type::write);
            wec  = ec;
            done = true;
            ioc.stop();
        };
        capy::run_async(ioc.get_executor())(body());
        capy::run_async(ioc.get_executor())(stop_guard(ioc, expired));
        ioc.run();
        BOOST_TEST(!expired);
        BOOST_TEST(done);
        BOOST_TEST(!wec);
        s1.close();
        s2.close();
    }

    void testWaitReactorPollFails()
    {
        io_context ioc(iocp);
        auto pair = make_socket_pair(ioc);
        auto& s1 = pair.first;
        auto& s2 = pair.second;
        std::optional<fault_scope> arm;
        std::error_code parked_ec;
        bool expired = false;
        auto parked = [&]() -> capy::task<>
        {
            // An error wait on a quiet socket never becomes ready, so
            // the only thing that can complete it is the reactor
            // leaving its loop.
            auto [ec] = co_await s1.wait(wait_type::error);
            parked_ec = ec;
            ioc.stop();
        };
        auto breaker = [&]() -> capy::task<>
        {
            // Armed after the wait above is queued, so whichever poll
            // fails first already has that op in hand: the drain on
            // the way out covers registered_ and pending_register_
            // alike.
            arm.emplace(sys::WSAPoll, WSAENOBUFS, 1u, any_thread);
            // A cancel for an op the reactor never registered is a
            // no-op that still pokes the self-pipe.
            s2.cancel();
            co_return;
        };
        capy::run_async(ioc.get_executor())(parked());
        capy::run_async(ioc.get_executor())(breaker());
        capy::run_async(ioc.get_executor())(stop_guard(ioc, expired));
        ioc.run();
        BOOST_TEST(!expired);
        BOOST_TEST(arm.has_value() && arm->fired());
        BOOST_TEST(parked_ec == capy::error::canceled);
        arm.reset();
        s1.close();
        s2.close();
    }

    void testLocalSetupFails()
    {
        io_context ioc(iocp);
        {
            local_stream_socket s(ioc);
            fault_scope f(sys::WSASocketW, WSAEAFNOSUPPORT);
            auto ec = s.open();
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::address_family_not_supported);
            BOOST_TEST(!s.is_open());
        }
        expect_no_handle_leak([&]{
            local_stream_socket s(ioc);
            fault_scope f(sys::CreateIoCompletionPort,
                ERROR_INVALID_PARAMETER);
            auto ec = s.open();
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == win_err(ERROR_INVALID_PARAMETER));
            BOOST_TEST(!s.is_open());
        });
        {
            local_stream_acceptor acc(ioc);
            fault_scope f(sys::WSASocketW, WSAEAFNOSUPPORT);
            auto ec = acc.open();
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::address_family_not_supported);
            BOOST_TEST(!acc.is_open());
        }
        expect_no_handle_leak([&]{
            local_stream_acceptor acc(ioc);
            fault_scope f(sys::CreateIoCompletionPort,
                ERROR_INVALID_PARAMETER);
            auto ec = acc.open();
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == win_err(ERROR_INVALID_PARAMETER));
            BOOST_TEST(!acc.is_open());
        });
        {
            auto h = make_native_socket(AF_UNIX, SOCK_STREAM);
            make_native_adoptable(h);
            expect_no_handle_leak([&]{
                {
                    // Adoption learns the family and type from
                    // SO_PROTOCOL_INFOW here too.
                    local_stream_socket s(ioc);
                    fault_scope f(sys::getsockopt, WSAENOTSOCK);
                    BOOST_TEST(s.assign(h) == std::errc::not_a_socket);
                    BOOST_TEST(f.fired());
                    BOOST_TEST(!s.is_open());
                }
                {
                    local_stream_socket s(ioc);
                    fault_scope f(sys::CreateIoCompletionPort,
                        ERROR_INVALID_PARAMETER);
                    BOOST_TEST(
                        s.assign(h) == win_err(ERROR_INVALID_PARAMETER));
                    BOOST_TEST(f.fired());
                    BOOST_TEST(!s.is_open());
                }
            });
            // A rejected adoption leaves the socket with the caller.
            BOOST_TEST(native_socket_valid(h));
            close_native_socket(h);
        }
        temp_socket_dir dir;
        auto const ep = corosio::local_endpoint(dir.path());
        {
            local_stream_acceptor acc(ioc);
            BOOST_TEST(!acc.open());
            // The unlink that precedes a bind is best effort: a
            // missing file is the common case
            // (local_stream_acceptor::bind).
            fault_scope f(sys::DeleteFileA, ERROR_ACCESS_DENIED);
            BOOST_TEST(!acc.bind(ep, bind_option::unlink_existing));
            BOOST_TEST(f.fired());
            BOOST_TEST(acc.is_open());
        }
        {
            local_stream_acceptor acc(ioc);
            BOOST_TEST(!acc.open());
            fault_scope f(sys::bind, WSAEADDRINUSE);
            BOOST_TEST(acc.bind(ep) == std::errc::address_in_use);
            BOOST_TEST(f.fired());
            BOOST_TEST(acc.is_open());
        }
        {
            local_stream_acceptor acc(ioc);
            BOOST_TEST(!acc.open());
            BOOST_TEST(!acc.bind(ep, bind_option::unlink_existing));
            fault_scope f(sys::listen, WSAEOPNOTSUPP);
            BOOST_TEST(acc.listen() == std::errc::operation_not_supported);
            BOOST_TEST(f.fired());
            // Not latched: the acceptor is still bound and open, so a
            // second listen takes.
            BOOST_TEST(acc.is_open());
            BOOST_TEST(!acc.listen());
        }
    }

    void testLocalConnectAcceptFails()
    {
        io_context ioc(iocp);
        temp_socket_dir dir;
        auto const ep = corosio::local_endpoint(dir.path());
        local_stream_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(ep, bind_option::unlink_existing));
        BOOST_TEST(!acc.listen());
        std::error_code bec, sockec, portec, syncec, compec;
        bool expired = false;
        auto body = [&]() -> capy::task<>
        {
            {
                // AF_UNIX ConnectEx also needs a bound socket, which
                // it satisfies with a family-only sockaddr_un.
                local_stream_socket s(ioc);
                BOOST_TEST(!s.open());
                fault_scope f(sys::bind, WSAEADDRNOTAVAIL);
                auto [ec] = co_await s.connect(ep);
                bec = ec;
                BOOST_TEST(f.fired());
            }
            local_stream_socket peer(ioc);
            {
                local_stream_socket c(ioc);
                BOOST_TEST(!c.open());
                auto [cec] = co_await c.connect(ep);
                BOOST_TEST(!cec);
                fault_scope f(sys::WSASocketW, WSAEAFNOSUPPORT);
                auto [ec] = co_await acc.accept(peer);
                sockec = ec;
                BOOST_TEST(f.fired());
                BOOST_TEST(!peer.is_open());
                c.cancel();
                c.close();
            }
            {
                local_stream_socket c(ioc);
                BOOST_TEST(!c.open());
                auto [cec] = co_await c.connect(ep);
                BOOST_TEST(!cec);
                fault_scope f(sys::CreateIoCompletionPort,
                    ERROR_INVALID_PARAMETER);
                auto [ec] = co_await acc.accept(peer);
                portec = ec;
                BOOST_TEST(f.fired());
                BOOST_TEST(!peer.is_open());
                c.cancel();
                c.close();
            }
            if(hook_is_live(sys::AcceptEx))
            {
                // The AF_UNIX accept reaches the same substituted
                // pointer as the tcp one.
                local_stream_socket c(ioc);
                BOOST_TEST(!c.open());
                auto [cec] = co_await c.connect(ep);
                BOOST_TEST(!cec);
                fault_scope f(sys::AcceptEx, WSAENOTSOCK);
                auto [ec] = co_await acc.accept(peer);
                syncec = ec;
                BOOST_TEST(f.fired());
                BOOST_TEST(!peer.is_open());
                c.cancel();
                c.close();
            }
            else
            {
                skip_dead_hook("AcceptEx");
            }
            {
                local_stream_socket c(ioc);
                BOOST_TEST(!c.open());
                auto [cec] = co_await c.connect(ep);
                BOOST_TEST(!cec);
                completion_fault_scope q(ERROR_NETNAME_DELETED);
                auto [ec] = co_await acc.accept(peer);
                compec = ec;
                BOOST_TEST(q.fired());
                BOOST_TEST(!peer.is_open());
                c.cancel();
                c.close();
            }
            ioc.stop();
        };
        capy::run_async(ioc.get_executor())(body());
        capy::run_async(ioc.get_executor())(stop_guard(ioc, expired));
        ioc.run();
        BOOST_TEST(!expired);
        BOOST_TEST(bec == std::errc::address_not_available);
        BOOST_TEST(sockec == std::errc::address_family_not_supported);
        BOOST_TEST(portec == win_err(ERROR_INVALID_PARAMETER));
        if(hook_is_live(sys::AcceptEx))
            BOOST_TEST(syncec == std::errc::not_a_socket);
        BOOST_TEST(compec == std::errc::connection_aborted);
    }

    void run()
    {
        testSchedulerConstructFails();
        testTimerCreationFails();
        testTimerThreadWaitFails();
        testStopPostFails();
        testPostFallbackRuns();
        testRunLoopDequeueFails();
        testTcpOpenFails();
        testTcpAssignFails();
        testTcpBindFails();
        testTcpOptionsFail();
        testTcpReleaseIgnoresDissociate();
        testTcpExtensionPointerMissing();
        testTcpConnectFails();
        testTcpReadWriteFails();
        testAcceptorOpenFails();
        testAcceptFails();
        testUdpSetupFails();
        testUdpIoFails();
        testWaitReactorSetupFails();
        testWaitReactorStartsOnFirstWait();
        testWaitReactorPollFails();
        testLocalSetupFails();
        testLocalConnectAcceptFails();
    }
};

TEST_SUITE(iocp_faults, "boost.corosio.fault.iocp");

} // boost::corosio::test::fault

#endif
