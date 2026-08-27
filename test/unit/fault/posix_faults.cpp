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

#include <boost/corosio/host_name.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/random_access_file.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/signal_set.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/stream_file.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>

#include <netdb.h>
#include <unistd.h>

namespace boost::corosio::test::fault {

template<auto Backend>
struct posix_common_faults
{
    // The io_uring backend has its own file implementation: reads and
    // writes go through the ring instead of preadv/pwritev, seek uses
    // lseek instead of fstat, and open never stats an appending file.
#if BOOST_COROSIO_HAS_IO_URING
    static constexpr bool ring_files = std::is_same_v<
        std::remove_cvref_t<decltype(Backend)>, io_uring_t>;
#else
    static constexpr bool ring_files = false;
#endif

    // macOS has no fdatasync, so sync_data() lowers to fsync there
    // (posix_stream_file::sync_data and
    // posix_random_access_file::sync_data). Keying off the library's
    // own macro rather than __APPLE__ keeps the two in step.
#if BOOST_COROSIO_HAS_POSIX_SYNCHRONIZED_IO
    static constexpr sys sync_data_call = sys::fdatasync;
#else
    static constexpr sys sync_data_call = sys::fsync;
#endif

    // Functional probe: proves the library's own socket() call binds
    // to the hook on this backend, whatever the link mode.
    void testSocketOpenFails()
    {
        io_context ioc(Backend);
        tcp_socket s(ioc);
        fault_scope f(sys::socket, EMFILE);
        auto ec = s.open(tcp::v4());
        BOOST_TEST(f.fired());
        BOOST_TEST(ec == std::errc::too_many_files_open);
        BOOST_TEST(!s.is_open());
    }

    void testBindFails()
    {
        io_context ioc(Backend);
        tcp_socket s(ioc);
        BOOST_TEST(!s.open(tcp::v4()));
        fault_scope f(sys::bind, EADDRINUSE);
        auto ec = s.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(f.fired());
        BOOST_TEST(ec == std::errc::address_in_use);
        BOOST_TEST(s.is_open());
    }

    void testSetOptionFails()
    {
        io_context ioc(Backend);
        tcp_socket s(ioc);
        BOOST_TEST(!s.open(tcp::v4()));
        fault_scope f(sys::setsockopt, ENOPROTOOPT);
        expect_system_error(
            [&]{ s.set_option(socket_option::reuse_address(true)); },
            std::errc::no_protocol_option);
        BOOST_TEST(f.fired());
        BOOST_TEST(s.is_open());
    }

    void testGetOptionFails()
    {
        io_context ioc(Backend);
        tcp_socket s(ioc);
        BOOST_TEST(!s.open(tcp::v4()));
        fault_scope f(sys::getsockopt, ENOPROTOOPT);
        expect_system_error(
            [&]{ std::ignore = s.get_option<socket_option::reuse_address>(); },
            std::errc::no_protocol_option);
        BOOST_TEST(f.fired());
        BOOST_TEST(s.is_open());
    }

    void testAssignValidateFails()
    {
        io_context ioc(Backend);
        int before = open_fds();
        auto h = make_native_socket(AF_INET, SOCK_STREAM);
        make_native_adoptable(h);
        {
            tcp_socket s(ioc);
            fault_scope f(sys::getsockname, EBADF);
            auto ec = s.assign(h);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::bad_file_descriptor);
            BOOST_TEST(!s.is_open());
        }
        {
            tcp_socket s(ioc);
            fault_scope f(sys::getsockopt, EBADF);
            auto ec = s.assign(h);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::bad_file_descriptor);
            BOOST_TEST(!s.is_open());
        }
        BOOST_TEST(native_socket_valid(h));
        close_native_socket(h);
        BOOST_TEST_EQ(open_fds(), before);
    }

    void testConnectPairFails()
    {
        io_context ioc(Backend);
        int before = open_fds();
        {
            local_stream_socket a(ioc), b(ioc);
            fault_scope f(sys::socketpair, EMFILE);
            auto ec = connect_pair(a, b);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::too_many_files_open);
            BOOST_TEST(!a.is_open() && !b.is_open());
        }
        // 1 is the F_GETFL probe, 2 the F_SETFL that follows it.
        for(unsigned nth : {1u, 2u})
        {
            local_stream_socket a(ioc), b(ioc);
            fault_scope f(sys::fcntl, EINVAL, nth);
            auto ec = connect_pair(a, b);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::invalid_argument);
            BOOST_TEST(!a.is_open() && !b.is_open());
        }
        // assign() interrogates the descriptor once with
        // getsockopt(SO_TYPE), so the ordinal selects which half of
        // the pair is refused. Either way both descriptors go back.
        for(unsigned nth : {1u, 2u})
        {
            local_stream_socket a(ioc), b(ioc);
            fault_scope f(sys::getsockopt, EBADF, nth);
            auto ec = connect_pair(a, b);
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(f.count(), nth);
            BOOST_TEST(ec == std::errc::bad_file_descriptor);
            BOOST_TEST(!a.is_open() && !b.is_open());
        }
        {
            local_datagram_socket a(ioc), b(ioc);
            fault_scope f(sys::socketpair, EMFILE);
            auto ec = connect_pair(a, b);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::too_many_files_open);
            BOOST_TEST(!a.is_open() && !b.is_open());
        }
        {
            local_datagram_socket a(ioc), b(ioc);
            BOOST_TEST(!connect_pair(a, b));
        }
        BOOST_TEST_EQ(open_fds(), before);
    }

    void testDatagramAvailableThrows()
    {
        io_context ioc(Backend);
        local_datagram_socket a(ioc), b(ioc);
        BOOST_TEST(!connect_pair(a, b));
        fault_scope f(sys::ioctl, EBADF);
        expect_system_error(
            [&]{ std::ignore = a.available(); },
            std::errc::bad_file_descriptor);
        BOOST_TEST(f.fired());
        BOOST_TEST(a.is_open());
    }

    void testDatagramBindFails()
    {
        io_context ioc(Backend);
        auto path = temp_path("ldb");
        ::unlink(path.c_str());
        local_datagram_socket s(ioc);
        BOOST_TEST(!s.open());
        // An already-open socket takes the early return rather than
        // replacing the descriptor it holds.
        BOOST_TEST(!s.open());
        {
            fault_scope f(sys::bind, EACCES);
            BOOST_TEST(s.bind(corosio::local_endpoint(path)) ==
                std::errc::permission_denied);
            BOOST_TEST(f.fired());
            BOOST_TEST(s.is_open());
        }
        BOOST_TEST(!s.bind(corosio::local_endpoint(path)));
        s.close();
        ::unlink(path.c_str());
    }

    /* The convenience constructors report through an exception, since
       there is no object yet to hand an error code back on. Each leg
       has its own `what`, which is the only way a caller can tell
       which step of open/bind/listen refused.
    */
    void testAcceptorConstructorThrows()
    {
        io_context ioc(Backend);
        {
            int before = open_fds();
            fault_scope f(sys::listen, EADDRINUSE);
            expect_system_error(
                [&]{
                    tcp_acceptor acc(
                        ioc, endpoint(ipv4_address::loopback(), 0));
                },
                std::errc::address_in_use);
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(open_fds(), before);
        }
        auto path = temp_path("lsa");
        {
            ::unlink(path.c_str());
            int before = open_fds();
            fault_scope f(sys::socket, EMFILE);
            expect_system_error(
                [&]{
                    local_stream_acceptor acc(
                        ioc, corosio::local_endpoint(path));
                },
                std::errc::too_many_files_open);
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(open_fds(), before);
        }
        {
            // bind() leaves the socket node behind, and a second bind
            // to a path that exists fails for its own reason.
            ::unlink(path.c_str());
            int before = open_fds();
            fault_scope f(sys::listen, EADDRINUSE);
            expect_system_error(
                [&]{
                    local_stream_acceptor acc(
                        ioc, corosio::local_endpoint(path));
                },
                std::errc::address_in_use);
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(open_fds(), before);
        }
        ::unlink(path.c_str());
    }

    void testAvailableThrows()
    {
        io_context ioc(Backend);
        local_stream_socket a(ioc), b(ioc);
        BOOST_TEST(!connect_pair(a, b));
        fault_scope f(sys::ioctl, ENOTTY);
        expect_system_error(
            [&]{ std::ignore = a.available(); },
            std::errc::inappropriate_io_control_operation);
        BOOST_TEST(f.fired());
        BOOST_TEST(a.is_open());
    }

    void testHostNameFails()
    {
        fault_scope f(sys::gethostname, EPERM);
        auto [ec, name] = host_name();
        BOOST_TEST(f.fired());
        BOOST_TEST(ec == std::errc::operation_not_permitted);
        BOOST_TEST(name.empty());
    }

    void testStreamFileOpenFails()
    {
        io_context ioc(Backend);
        auto path = temp_path("sf");
        stream_file sf(ioc);
        {
            fault_scope f(sys::open, EACCES);
            auto ec = sf.open(path, file_base::read_write | file_base::create);
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::permission_denied);
            BOOST_TEST(!sf.is_open());
        }
        // Only the POSIX backend seeds its own offset with fstat when
        // opening for append; the ring backend leaves that to O_APPEND.
        if constexpr(!ring_files)
        {
            // fstat is unshadowed on pre-2.33 glibc (an inline redirect to
            // __fxstat), so the append-offset seed cannot be faulted there.
            if(!hook_is_live(sys::fstat))
            {
                skip_dead_hook("fstat");
            }
            else
            {
                int before = open_fds();
                fault_scope f(sys::fstat, EIO);
                auto ec = sf.open(path, file_base::write_only |
                    file_base::create | file_base::append);
                BOOST_TEST(f.fired());
                BOOST_TEST(ec == std::errc::io_error);
                BOOST_TEST(!sf.is_open());
                BOOST_TEST_EQ(open_fds(), before);
            }
        }
        ::unlink(path.c_str());
    }

    void testStreamFileSyncOps()
    {
        io_context ioc(Backend);
        auto path = temp_path("sf2");
        stream_file sf(ioc);
        BOOST_TEST(!sf.open(path, file_base::read_write | file_base::create));
        if(!hook_is_live(sys::fstat))
        {
            skip_dead_hook("fstat");
        }
        else
        {
            fault_scope f(sys::fstat, EIO);
            expect_system_error(
                [&]{ std::ignore = sf.size(); }, std::errc::io_error);
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sys::ftruncate, EFBIG);
            BOOST_TEST(sf.resize(16) == std::errc::file_too_large);
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sync_data_call, EIO);
            BOOST_TEST(sf.sync_data() == std::errc::io_error);
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sys::fsync, EIO);
            BOOST_TEST(sf.sync_all() == std::errc::io_error);
            BOOST_TEST(f.fired());
        }
        {
            constexpr sys seek_end_call = ring_files ? sys::lseek : sys::fstat;
            if(!hook_is_live(seek_end_call))
            {
                skip_dead_hook(ring_files ? "lseek" : "fstat");
            }
            else
            {
                fault_scope f(seek_end_call, EIO);
                auto [ec, pos] = sf.seek(0, file_base::seek_end);
                BOOST_TEST(f.fired());
                BOOST_TEST(ec == std::errc::io_error);
                BOOST_TEST_EQ(pos, 0u);
            }
        }
        sf.close();
        ::unlink(path.c_str());
    }

    void testStreamFileIoFails()
    {
        // Ring file I/O never reaches preadv/pwritev; its completion
        // faults belong with the other io_uring coverage.
        if constexpr(ring_files)
            return;
        io_context ioc(Backend);
        auto path = temp_path("sf3");
        stream_file sf(ioc);
        BOOST_TEST(!sf.open(path, file_base::read_write | file_base::create));
        char buf[8] = "1234567";
        std::error_code rec, wec, eec;
        std::size_t rn = 99, en = 99;
        auto t = [&]() -> capy::task<>
        {
            // The syscall runs on a pool thread, so the arm has to be
            // visible outside this one.
            {
                fault_scope f(sys::pwritev, ENOSPC, 1, any_thread);
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
                fault_scope f(sys::preadv, EIO, 1, any_thread);
                auto [ec, n] = co_await sf.read_some(
                    capy::mutable_buffer(buf, 7));
                rec = ec;
                rn = n;
                BOOST_TEST(f.fired());
            }
            {
                auto f = fault_scope::returning_any_thread(sys::preadv, 0);
                auto [ec, n] = co_await sf.read_some(
                    capy::mutable_buffer(buf, 7));
                eec = ec;
                en = n;
                BOOST_TEST(f.fired());
            }
        };
        capy::run_async(ioc.get_executor())(t());
        ioc.run();
        BOOST_TEST(wec == std::errc::no_space_on_device);
        BOOST_TEST(rec == std::errc::io_error);
        BOOST_TEST_EQ(rn, 0u);
        BOOST_TEST(eec == capy::error::eof);
        BOOST_TEST_EQ(en, 0u);
        sf.close();
        ::unlink(path.c_str());
    }

    void testRandomAccessFileFails()
    {
        io_context ioc(Backend);
        auto path = temp_path("raf");
        random_access_file rf(ioc);
        {
            fault_scope f(sys::open, EACCES);
            BOOST_TEST(rf.open(path, file_base::read_write |
                file_base::create) == std::errc::permission_denied);
            BOOST_TEST(f.fired());
        }
        BOOST_TEST(!rf.open(path, file_base::read_write | file_base::create));
        if(!hook_is_live(sys::fstat))
        {
            skip_dead_hook("fstat");
        }
        else
        {
            fault_scope f(sys::fstat, EIO);
            expect_system_error(
                [&]{ std::ignore = rf.size(); }, std::errc::io_error);
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sys::ftruncate, EFBIG);
            BOOST_TEST(rf.resize(16) == std::errc::file_too_large);
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sync_data_call, EIO);
            BOOST_TEST(rf.sync_data() == std::errc::io_error);
            BOOST_TEST(f.fired());
        }
        {
            fault_scope f(sys::fsync, EIO);
            BOOST_TEST(rf.sync_all() == std::errc::io_error);
            BOOST_TEST(f.fired());
        }
        // Ring file I/O never reaches preadv/pwritev; its completion
        // faults belong with the other io_uring coverage.
        if constexpr(ring_files)
        {
            rf.close();
            ::unlink(path.c_str());
            return;
        }
        char buf[8] = "1234567";
        std::error_code rec, wec, eec;
        auto t = [&]() -> capy::task<>
        {
            {
                fault_scope f(sys::pwritev, ENOSPC, 1, any_thread);
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
                fault_scope f(sys::preadv, EIO, 1, any_thread);
                auto [ec, n] = co_await rf.read_some_at(
                    0, capy::mutable_buffer(buf, 7));
                std::ignore = n;
                rec = ec;
                BOOST_TEST(f.fired());
            }
            {
                auto f = fault_scope::returning_any_thread(sys::preadv, 0);
                auto [ec, n] = co_await rf.read_some_at(
                    0, capy::mutable_buffer(buf, 7));
                std::ignore = n;
                eec = ec;
                BOOST_TEST(f.fired());
            }
        };
        capy::run_async(ioc.get_executor())(t());
        ioc.run();
        BOOST_TEST(wec == std::errc::no_space_on_device);
        BOOST_TEST(rec == std::errc::io_error);
        BOOST_TEST(eec == capy::error::eof);
        rf.close();
        ::unlink(path.c_str());
    }

    void testResolverFails()
    {
        io_context ioc(Backend);
        resolver r(ioc);
        std::error_code fec, rec;
        auto t = [&]() -> capy::task<>
        {
            {
                fault_scope f(sys::getaddrinfo, EAI_FAIL, 1, any_thread);
                auto [ec, results] = co_await r.resolve("localhost", "80");
                std::ignore = results;
                fec = ec;
                BOOST_TEST(f.fired());
            }
            {
                fault_scope f(sys::getnameinfo, EAI_FAIL, 1, any_thread);
                auto [ec, result] = co_await r.resolve(
                    endpoint(ipv4_address::loopback(), 80));
                std::ignore = result;
                rec = ec;
                BOOST_TEST(f.fired());
            }
        };
        capy::run_async(ioc.get_executor())(t());
        ioc.run();
        BOOST_TEST(fec == std::errc::io_error);
        BOOST_TEST(rec == std::errc::io_error);
    }

    // The signal service's shutdown walks the implementations it still
    // owns, deleting each set and the registrations hanging off it. A
    // signal set that outlives its io_context is the only way to reach
    // that walk, and the SIGINT registration it leaves behind stays in
    // the process signal table and fails every later add() of the same
    // signal -- so the whole thing happens in a child that dies with it.
    void testSignalTeardownWalk()
    {
        in_child([]{
            bool resumed = false;
            {
                io_context ioc(Backend);
                auto keeper = [&]() -> capy::task<> {
                    signal_set sig(ioc, SIGINT);
                    std::ignore = co_await sig.wait();
                    resumed = true;
                };
                capy::run_async(ioc.get_executor())(keeper());
                // Exactly one handler, and it has to be the coroutine
                // start: a zero here would leave nothing parked and the
                // walk with nothing to reclaim.
                if(ioc.run_one() != 1)
                    return false;
            }
            return !resumed;
        });
    }

    void run()
    {
        if(skip_under_valgrind())
            return;
        testSocketOpenFails();
        testBindFails();
        testSetOptionFails();
        testGetOptionFails();
        testAssignValidateFails();
        testConnectPairFails();
        testDatagramAvailableThrows();
        testDatagramBindFails();
        testAcceptorConstructorThrows();
        testAvailableThrows();
        testHostNameFails();
        testStreamFileOpenFails();
        testStreamFileSyncOps();
        testStreamFileIoFails();
        testRandomAccessFileFails();
        testResolverFails();
        testSignalTeardownWalk();
    }
};

COROSIO_NON_IOCP_BACKEND_TESTS(posix_common_faults, "boost.corosio.fault.posix")

template<auto Backend>
struct reactor_acceptor_option_faults
{
    void testAcceptorOptions()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        {
            fault_scope f(sys::setsockopt, ENOPROTOOPT);
            expect_system_error(
                [&]{ acc.set_option(socket_option::reuse_address(true)); },
                std::errc::no_protocol_option);
            BOOST_TEST(f.fired());
            BOOST_TEST(acc.is_open());
        }
        {
            fault_scope f(sys::getsockopt, ENOPROTOOPT);
            expect_system_error(
                [&]{
                    std::ignore =
                        acc.get_option<socket_option::reuse_address>();
                },
                std::errc::no_protocol_option);
            BOOST_TEST(f.fired());
            BOOST_TEST(acc.is_open());
        }
    }

    void run()
    {
        if(skip_under_valgrind())
            return;
        testAcceptorOptions();
    }
};

COROSIO_REACTOR_BACKEND_TESTS(
    reactor_acceptor_option_faults, "boost.corosio.fault.posix.acceptor_opts")

} // boost::corosio::test::fault
