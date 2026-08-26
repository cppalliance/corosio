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

#include <boost/corosio/host_name.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/random_access_file.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/signal_set.hpp>
#include <boost/corosio/stream_file.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <csignal>
#include <filesystem>
#include <string>
#include <system_error>
#include <tuple>

#if BOOST_COROSIO_HAS_IOCP

namespace boost::corosio::test::fault {

namespace {

void remove_file(std::string const& path)
{
    std::error_code ec;
    std::ignore = std::filesystem::remove(std::filesystem::path(path), ec);
}

} // namespace

/* Faults on the Windows entry points that are not the IOCP backend's
   own: the file services, the resolver, host_name and the CRT signal
   registration. The IOCP socket paths live in iocp_faults.cpp.
*/
struct win_common_faults
{
    void testHostNameFails()
    {
        {
            // The size query is the only call that reports through
            // GetLastError alone, and host_name passes its code
            // through untouched.
            fault_scope f(sys::GetComputerNameExW, ERROR_ACCESS_DENIED);
            auto [ec, name] = host_name();
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == win_err(ERROR_ACCESS_DENIED));
            BOOST_TEST(name.empty());
        }
        {
            // 1 is the size query, which has to report ERROR_MORE_DATA
            // for the fetch to happen at all.
            fault_scope f(sys::GetComputerNameExW, ERROR_INVALID_PARAMETER, 2);
            auto [ec, name] = host_name();
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == win_err(ERROR_INVALID_PARAMETER));
            BOOST_TEST(name.empty());
        }
        {
            // Sizing conversion: returns 0, so `needed <= 0`.
            fault_scope f(sys::WideCharToMultiByte, ERROR_INSUFFICIENT_BUFFER);
            auto [ec, name] = host_name();
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == win_err(ERROR_INSUFFICIENT_BUFFER));
            BOOST_TEST(name.empty());
        }
        {
            // Converting conversion: 0 written where `needed` was
            // positive.
            fault_scope f(sys::WideCharToMultiByte, ERROR_INVALID_PARAMETER, 2);
            auto [ec, name] = host_name();
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == win_err(ERROR_INVALID_PARAMETER));
            BOOST_TEST(name.empty());
        }
        // Unfaulted, the call must still work: the arms above are the
        // only reason any of them failed.
        auto [ec, name] = host_name();
        BOOST_TEST(!ec);
        BOOST_TEST(!name.empty());
    }

    void testStreamFileOpenFails()
    {
        io_context ioc(iocp);
        auto path = temp_path("winsf");
        stream_file sf(ioc);
        {
            fault_scope f(sys::CreateFileW, ERROR_ACCESS_DENIED);
            auto ec = sf.open(path, file_base::read_write |
                file_base::create);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == win_err(ERROR_ACCESS_DENIED));
            BOOST_TEST(!sf.is_open());
        }
        // The handle exists when the association fails, so the
        // failure path owns closing it.
        expect_no_handle_leak([&]{
            fault_scope f(sys::CreateIoCompletionPort,
                ERROR_INVALID_PARAMETER);
            auto ec = sf.open(path, file_base::read_write |
                file_base::create);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == win_err(ERROR_INVALID_PARAMETER));
            BOOST_TEST(!sf.is_open());
        });
        // create|truncate lowers to OPEN_ALWAYS plus an explicit
        // SetEndOfFile; every other mode leaves it to the disposition.
        expect_no_handle_leak([&]{
            fault_scope f(sys::SetEndOfFile, ERROR_DISK_FULL);
            auto ec = sf.open(path, file_base::read_write |
                file_base::create | file_base::truncate);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == win_err(ERROR_DISK_FULL));
            BOOST_TEST(!sf.is_open());
        });
        // Only an appending open seeds its own offset from the file
        // size.
        expect_no_handle_leak([&]{
            fault_scope f(sys::GetFileSizeEx, ERROR_INVALID_HANDLE);
            auto ec = sf.open(path, file_base::write_only |
                file_base::create | file_base::append);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == win_err(ERROR_INVALID_HANDLE));
            BOOST_TEST(!sf.is_open());
        });
        remove_file(path);
    }

    void testStreamFileSyncOps()
    {
        io_context ioc(iocp);
        auto path = temp_path("winsf2");
        stream_file sf(ioc);
        BOOST_TEST(!sf.open(path, file_base::read_write | file_base::create));
        {
            fault_scope f(sys::GetFileSizeEx, ERROR_INVALID_HANDLE);
            expect_system_error(
                [&]{ std::ignore = sf.size(); },
                win_err(ERROR_INVALID_HANDLE));
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sys::SetFilePointerEx, ERROR_INVALID_PARAMETER);
            BOOST_TEST(sf.resize(16) == win_err(ERROR_INVALID_PARAMETER));
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sys::SetEndOfFile, ERROR_DISK_FULL);
            BOOST_TEST(sf.resize(16) == win_err(ERROR_DISK_FULL));
            BOOST_TEST(f.fired());
        }
        {
            // sync_data tries the data-only NT flush first and only
            // falls back to FlushFileBuffers when that fails, so the
            // fallback needs both arms. Where the NT entry point never
            // resolved, its arm has nothing to fail and the fallback
            // was already the only path.
            fault_scope nt(sys::NtFlushBuffersFileEx, ERROR_INVALID_FUNCTION);
            fault_scope f(sys::FlushFileBuffers, ERROR_WRITE_FAULT);
            BOOST_TEST(sf.sync_data() == win_err(ERROR_WRITE_FAULT));
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sys::FlushFileBuffers, ERROR_WRITE_FAULT);
            BOOST_TEST(sf.sync_all() == win_err(ERROR_WRITE_FAULT));
            BOOST_TEST(f.fired());
        }
        {
            // Only seek_end asks the file for its size.
            fault_scope f(sys::GetFileSizeEx, ERROR_INVALID_HANDLE);
            auto [ec, pos] = sf.seek(0, file_base::seek_end);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == win_err(ERROR_INVALID_HANDLE));
            BOOST_TEST_EQ(pos, 0u);
        }
        sf.close();
        remove_file(path);
    }

    void testStreamFileIoFails()
    {
        io_context ioc(iocp);
        auto path = temp_path("winsf3");
        stream_file sf(ioc);
        BOOST_TEST(!sf.open(path, file_base::read_write | file_base::create));
        char buf[8] = "1234567";
        std::error_code wec, rec, cec, eec;
        std::size_t rn = 99, en = 99;
        auto t = [&]() -> capy::task<>
        {
            {
                fault_scope f(sys::WriteFile, ERROR_ACCESS_DENIED);
                auto [ec, n] = co_await sf.write_some(
                    capy::const_buffer(buf, 7));
                std::ignore = n;
                wec = ec;
                BOOST_TEST(f.fired());
            }
            {
                auto [ec, n] = co_await sf.write_some(
                    capy::const_buffer(buf, 7));
                std::ignore = n;
                BOOST_TEST(!ec);
            }
            {
                auto [ec, pos] = sf.seek(0, file_base::seek_set);
                std::ignore = pos;
                BOOST_TEST(!ec);
            }
            {
                fault_scope f(sys::ReadFile, ERROR_ACCESS_DENIED);
                auto [ec, n] = co_await sf.read_some(
                    capy::mutable_buffer(buf, 7));
                rec = ec;
                rn = n;
                BOOST_TEST(f.fired());
            }
            {
                // The kernel result of a queued read cannot be armed
                // where it was started; the completion carries it.
                completion_fault_scope q(ERROR_LOCK_VIOLATION);
                auto [ec, n] = co_await sf.read_some(
                    capy::mutable_buffer(buf, 7));
                std::ignore = n;
                cec = ec;
                BOOST_TEST(q.fired());
            }
            {
                // A zero-length ReadFile completes with zero bytes,
                // which the stream contract reads as end of file.
                auto f = fault_scope::returning(sys::ReadFile, 0);
                auto [ec, n] = co_await sf.read_some(
                    capy::mutable_buffer(buf, 7));
                eec = ec;
                en = n;
                BOOST_TEST(f.fired());
            }
        };
        capy::run_async(ioc.get_executor())(t());
        ioc.run();
        BOOST_TEST(wec == win_err(ERROR_ACCESS_DENIED));
        BOOST_TEST(rec == win_err(ERROR_ACCESS_DENIED));
        BOOST_TEST_EQ(rn, 0u);
        BOOST_TEST(cec == win_err(ERROR_LOCK_VIOLATION));
        BOOST_TEST(eec == capy::error::eof);
        BOOST_TEST_EQ(en, 0u);
        sf.close();
        remove_file(path);
    }

    void testRandomAccessFileFails()
    {
        io_context ioc(iocp);
        auto path = temp_path("winraf");
        random_access_file rf(ioc);
        {
            fault_scope f(sys::CreateFileW, ERROR_ACCESS_DENIED);
            BOOST_TEST(rf.open(path, file_base::read_write |
                file_base::create) == win_err(ERROR_ACCESS_DENIED));
            BOOST_TEST(f.fired());
        }
        expect_no_handle_leak([&]{
            fault_scope f(sys::CreateIoCompletionPort,
                ERROR_INVALID_PARAMETER);
            BOOST_TEST(rf.open(path, file_base::read_write |
                file_base::create) == win_err(ERROR_INVALID_PARAMETER));
            BOOST_TEST(f.fired());
        });
        BOOST_TEST(!rf.open(path, file_base::read_write | file_base::create));
        {
            fault_scope f(sys::GetFileSizeEx, ERROR_INVALID_HANDLE);
            expect_system_error(
                [&]{ std::ignore = rf.size(); },
                win_err(ERROR_INVALID_HANDLE));
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sys::SetFilePointerEx, ERROR_INVALID_PARAMETER);
            BOOST_TEST(rf.resize(16) == win_err(ERROR_INVALID_PARAMETER));
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sys::SetEndOfFile, ERROR_DISK_FULL);
            BOOST_TEST(rf.resize(16) == win_err(ERROR_DISK_FULL));
            BOOST_TEST(f.fired());
        }
        {
            fault_scope nt(sys::NtFlushBuffersFileEx, ERROR_INVALID_FUNCTION);
            fault_scope f(sys::FlushFileBuffers, ERROR_WRITE_FAULT);
            BOOST_TEST(rf.sync_data() == win_err(ERROR_WRITE_FAULT));
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sys::FlushFileBuffers, ERROR_WRITE_FAULT);
            BOOST_TEST(rf.sync_all() == win_err(ERROR_WRITE_FAULT));
            BOOST_TEST(f.fired());
        }
        char buf[8] = "1234567";
        std::error_code wec, rec, cec, eec;
        auto t = [&]() -> capy::task<>
        {
            {
                fault_scope f(sys::WriteFile, ERROR_ACCESS_DENIED);
                auto [ec, n] = co_await rf.write_some_at(
                    0, capy::const_buffer(buf, 7));
                std::ignore = n;
                wec = ec;
                BOOST_TEST(f.fired());
            }
            {
                auto [ec, n] = co_await rf.write_some_at(
                    0, capy::const_buffer(buf, 7));
                std::ignore = n;
                BOOST_TEST(!ec);
            }
            {
                fault_scope f(sys::ReadFile, ERROR_ACCESS_DENIED);
                auto [ec, n] = co_await rf.read_some_at(
                    0, capy::mutable_buffer(buf, 7));
                std::ignore = n;
                rec = ec;
                BOOST_TEST(f.fired());
            }
            {
                completion_fault_scope q(ERROR_LOCK_VIOLATION);
                auto [ec, n] = co_await rf.read_some_at(
                    0, capy::mutable_buffer(buf, 7));
                std::ignore = n;
                cec = ec;
                BOOST_TEST(q.fired());
            }
            {
                auto f = fault_scope::returning(sys::ReadFile, 0);
                auto [ec, n] = co_await rf.read_some_at(
                    0, capy::mutable_buffer(buf, 7));
                std::ignore = n;
                eec = ec;
                BOOST_TEST(f.fired());
            }
        };
        capy::run_async(ioc.get_executor())(t());
        ioc.run();
        BOOST_TEST(wec == win_err(ERROR_ACCESS_DENIED));
        BOOST_TEST(rec == win_err(ERROR_ACCESS_DENIED));
        BOOST_TEST(cec == win_err(ERROR_LOCK_VIOLATION));
        BOOST_TEST(eec == capy::error::eof);
        rf.close();
        remove_file(path);
    }

    void testConnectPairFails()
    {
        io_context ioc(iocp);
        // The pair is built by hand out of a listening AF_UNIX socket,
        // a worker that connects and a blocking accept here; each of
        // those four calls has its own failure path.
        {
            local_stream_socket a(ioc), b(ioc);
            fault_scope f(sys::WSASocketW, WSAEAFNOSUPPORT);
            auto ec = connect_pair(a, b);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::address_family_not_supported);
            BOOST_TEST(!a.is_open() && !b.is_open());
        }
        {
            local_stream_socket a(ioc), b(ioc);
            fault_scope f(sys::bind, WSAEADDRINUSE);
            auto ec = connect_pair(a, b);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::address_in_use);
            BOOST_TEST(!a.is_open() && !b.is_open());
        }
        {
            local_stream_socket a(ioc), b(ioc);
            fault_scope f(sys::listen, WSAEOPNOTSUPP);
            auto ec = connect_pair(a, b);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::operation_not_supported);
            BOOST_TEST(!a.is_open() && !b.is_open());
        }
        {
            // The worker still connects, so the accept fault is the
            // one caller-side failure that does not strand it.
            local_stream_socket a(ioc), b(ioc);
            fault_scope f(sys::accept, WSAENOTSOCK);
            auto ec = connect_pair(a, b);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::not_a_socket);
            BOOST_TEST(!a.is_open() && !b.is_open());
        }
        // Adoption of the first descriptor fails; both are the
        // library's to close.
        expect_no_handle_leak([&]{
            local_stream_socket a(ioc), b(ioc);
            fault_scope f(sys::getsockopt, WSAENOTSOCK);
            auto ec = connect_pair(a, b);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::not_a_socket);
            BOOST_TEST(!a.is_open() && !b.is_open());
        });
        // Unfaulted, a pair still forms.
        local_stream_socket a(ioc), b(ioc);
        BOOST_TEST(!connect_pair(a, b));
    }

    void testAvailableThrows()
    {
        io_context ioc(iocp);
        local_stream_socket a(ioc), b(ioc);
        BOOST_TEST(!connect_pair(a, b));
        // available() reports the raw Winsock code rather than
        // routing it through make_err
        // (src/corosio/src/local_stream_socket.cpp:118-123).
        fault_scope f(sys::ioctlsocket, WSAEINVAL);
        expect_system_error(
            [&]{ std::ignore = a.available(); }, win_err(WSAEINVAL));
        BOOST_TEST(f.fired());
        BOOST_TEST(a.is_open());
    }

    void testResolverFails()
    {
        io_context ioc(iocp);
        resolver r(ioc);
        std::error_code fec, rec;
        auto t = [&]() -> capy::task<>
        {
            {
                // GetAddrInfoExW reports a synchronous failure through
                // its return value and the last-error slot alike.
                fault_scope f(sys::GetAddrInfoExW, WSAEAFNOSUPPORT);
                auto [ec, results] = co_await r.resolve("localhost", "80");
                std::ignore = results;
                fec = ec;
                BOOST_TEST(f.fired());
            }
            {
                // GetNameInfoW blocks, so it runs on a pool thread
                // where the thread-local arms are never consulted.
                fault_scope f(sys::GetNameInfoW, WSAEAFNOSUPPORT, 1,
                    any_thread);
                auto [ec, result] = co_await r.resolve(
                    endpoint(ipv4_address::loopback(), 80));
                std::ignore = result;
                rec = ec;
                BOOST_TEST(f.fired());
            }
        };
        capy::run_async(ioc.get_executor())(t());
        ioc.run();
        BOOST_TEST(fec == std::errc::address_family_not_supported);
        BOOST_TEST(rec == std::errc::address_family_not_supported);
    }

    // resolver_detail::to_wide gives up when MultiByteToWideChar
    // reports a length of zero or less and hands back an empty string.
    // Faulting the length probe of each conversion in turn leaves both
    // the node and the service name empty, and win_resolver::resolve
    // passes a null pointer for an empty one -- a lookup with neither
    // is the documented WSAHOST_NOT_FOUND. The conversions run on the
    // calling thread, so a thread-local arm reaches them.
    void testResolverWideConversionFails()
    {
        io_context ioc(iocp);
        resolver r(ioc);
        std::error_code rec;
        bool armed_fired = false;
        auto t = [&]() -> capy::task<>
        {
            // The host conversion is calls 1 and 2 and the service
            // conversion 3 and 4; failing the first probe skips call 2,
            // so nth 2 is the service's own probe.
            fault_scope host(sys::MultiByteToWideChar,
                ERROR_INVALID_PARAMETER, 1);
            fault_scope service(sys::MultiByteToWideChar,
                ERROR_INVALID_PARAMETER, 2);
            auto [ec, results] = co_await r.resolve("localhost", "80");
            std::ignore = results;
            rec = ec;
            armed_fired = host.fired() && service.fired();
        };
        capy::run_async(ioc.get_executor())(t());
        ioc.run();
        BOOST_TEST(armed_fired);
        if(rec != win_err(WSAHOST_NOT_FOUND))
            std::fprintf(stderr,
                "fault harness: name-less lookup reported %d (%s)\n",
                rec.value(), rec.message().c_str());
        BOOST_TEST(rec == win_err(WSAHOST_NOT_FOUND));
    }

    void testResolverCancelIgnored()
    {
        io_context ioc(iocp);
        resolver r(ioc);
        std::error_code rec;
        bool cancel_fired = false;
        auto body = [&]() -> capy::task<>
        {
            auto [ec, results] = co_await r.resolve(
                "corosio-fault-nonexistent.invalid", "80");
            std::ignore = results;
            rec = ec;
        };
        auto canceller = [&]() -> capy::task<>
        {
            fault_scope f(sys::GetAddrInfoExCancel, WSAEINVAL);
            r.cancel();
            cancel_fired = f.fired();
            co_return;
        };
        capy::run_async(ioc.get_executor())(body());
        capy::run_async(ioc.get_executor())(canceller());
        ioc.run();
        // A lookup answered from the resolver cache completes before
        // the canceller runs and never records a cancel handle, so the
        // discarded return value is only asserted on the pending path.
        if(rec == capy::error::canceled)
            BOOST_TEST(cancel_fired);
    }

    void run()
    {
        testHostNameFails();
        testStreamFileOpenFails();
        testStreamFileSyncOps();
        testStreamFileIoFails();
        testRandomAccessFileFails();
        testConnectPairFails();
        testAvailableThrows();
        testResolverFails();
        testResolverWideConversionFails();
        testResolverCancelIgnored();
    }
};

TEST_SUITE(win_common_faults, "boost.corosio.fault.win");

/* The CRT's signal registration is process-wide and counted, so a
   second signal_set in the same process would find the handler already
   installed and never reach the faulted call. There is no fork on
   Windows to isolate that in, so these live in a suite of their own:
   CTest runs each suite as its own process.
*/
struct win_signal_faults
{
    // SIGTERM is defined by every Windows CRT and is never raised by
    // the OS, so registering and restoring it disturbs nothing.
    static constexpr int signum = SIGTERM;

    void testAddFails()
    {
        io_context ioc(iocp);
        signal_set ss(ioc);
        {
            fault_scope f(sys::signal, ERROR_INVALID_PARAMETER);
            auto ec = ss.add(signum);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::invalid_argument);
        }
        // Not latched: the failed add left no registration behind, so
        // the retry installs the handler.
        BOOST_TEST(!ss.add(signum));
        BOOST_TEST(!ss.clear());
    }

    void testRemoveFails()
    {
        io_context ioc(iocp);
        signal_set ss(ioc);
        BOOST_TEST(!ss.add(signum));
        {
            fault_scope f(sys::signal, ERROR_INVALID_PARAMETER);
            auto ec = ss.remove(signum);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::invalid_argument);
        }
        // The failed remove returns before unlinking, so the
        // registration is still there for the retry to take out
        // (win_signals::remove_signal).
        BOOST_TEST(!ss.remove(signum));
        BOOST_TEST(!ss.clear());
    }

    void testClearFails()
    {
        io_context ioc(iocp);
        signal_set ss(ioc);
        BOOST_TEST(!ss.add(signum));
        {
            fault_scope f(sys::signal, ERROR_INVALID_PARAMETER);
            auto ec = ss.clear();
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::invalid_argument);
        }
        // clear() reports the first failure but still unlinks every
        // registration, so the set is empty either way.
        BOOST_TEST(!ss.clear());
        BOOST_TEST(!ss.add(signum));
        BOOST_TEST(!ss.clear());
    }

    void run()
    {
        testAddFails();
        testRemoveFails();
        testClearFails();
    }
};

TEST_SUITE(win_signal_faults, "boost.corosio.fault.win.signals");

} // boost::corosio::test::fault

#endif
