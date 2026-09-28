//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Services are created on first use, not at context construction.
// Linking a program that never names a protocol must not require that
// protocol's service code, so constructing an io_context must not
// instantiate any I/O service eagerly.

#include <boost/corosio/io_context.hpp>

#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/posix_descriptor.hpp>
#include <boost/corosio/random_access_file.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/signal_set.hpp>
#include <boost/corosio/stream_file.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/udp_socket.hpp>

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/detail/timer_service.hpp>

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_file_service.hpp>
#include <boost/corosio/native/detail/iocp/win_local_stream_acceptor_service.hpp>
#include <boost/corosio/native/detail/iocp/win_local_stream_service.hpp>
#include <boost/corosio/native/detail/iocp/win_random_access_file_service.hpp>
#include <boost/corosio/native/detail/iocp/win_resolver_service.hpp>
#include <boost/corosio/native/detail/iocp/win_signals.hpp>
#include <boost/corosio/native/detail/iocp/win_tcp_acceptor_service.hpp>
#include <boost/corosio/native/detail/iocp/win_udp_service.hpp>
#else
#include <boost/corosio/detail/descriptor_service.hpp>
#include <boost/corosio/detail/file_service.hpp>
#include <boost/corosio/detail/local_datagram_service.hpp>
#include <boost/corosio/detail/local_stream_acceptor_service.hpp>
#include <boost/corosio/detail/local_stream_service.hpp>
#include <boost/corosio/detail/random_access_file_service.hpp>
#include <boost/corosio/detail/tcp_acceptor_service.hpp>
#include <boost/corosio/detail/tcp_service.hpp>
#include <boost/corosio/detail/udp_service.hpp>
#include <boost/corosio/native/detail/posix/posix_resolver_service.hpp>
#include <boost/corosio/native/detail/posix/posix_signal_service.hpp>
#endif

#include <memory>
#include <thread>
#include <vector>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

namespace {

// The service types the public objects look up, per platform.
#if BOOST_COROSIO_HAS_IOCP
using tcp_service_t          = detail::win_tcp_service;
using tcp_acceptor_service_t = detail::win_tcp_acceptor_service;
using udp_service_t          = detail::win_udp_service;
using local_stream_service_t = detail::win_local_stream_service;
using local_stream_acceptor_service_t =
    detail::win_local_stream_acceptor_service;
using resolver_service_t = detail::win_resolver_service;
using signal_service_t   = detail::win_signals;
using file_service_t     = detail::win_file_service;
using random_access_file_service_t =
    detail::win_random_access_file_service;
#else
using tcp_service_t          = detail::tcp_service;
using tcp_acceptor_service_t = detail::tcp_acceptor_service;
using udp_service_t          = detail::udp_service;
using local_stream_service_t = detail::local_stream_service;
using local_stream_acceptor_service_t =
    detail::local_stream_acceptor_service;
using local_datagram_service_t = detail::local_datagram_service;
using descriptor_service_t     = detail::descriptor_service;
using resolver_service_t       = detail::posix_resolver_service;
using signal_service_t         = detail::posix_signal_service;
using file_service_t           = detail::file_service;
using random_access_file_service_t = detail::random_access_file_service;
#endif

} // namespace

template<auto Backend>
struct lazy_services_test
{
    void testConstructionCreatesNoIoServices()
    {
        io_context ioc(Backend);

        BOOST_TEST(ioc.template find_service<tcp_service_t>() == nullptr);
        BOOST_TEST(
            ioc.template find_service<tcp_acceptor_service_t>() == nullptr);
        BOOST_TEST(ioc.template find_service<udp_service_t>() == nullptr);
        BOOST_TEST(
            ioc.template find_service<local_stream_service_t>() == nullptr);
        BOOST_TEST(
            ioc.template find_service<local_stream_acceptor_service_t>() ==
            nullptr);
#if !BOOST_COROSIO_HAS_IOCP
        BOOST_TEST(
            ioc.template find_service<local_datagram_service_t>() == nullptr);
        BOOST_TEST(
            ioc.template find_service<descriptor_service_t>() == nullptr);
#endif
        BOOST_TEST(ioc.template find_service<resolver_service_t>() == nullptr);
        BOOST_TEST(ioc.template find_service<signal_service_t>() == nullptr);
        BOOST_TEST(ioc.template find_service<file_service_t>() == nullptr);
        BOOST_TEST(
            ioc.template find_service<random_access_file_service_t>() ==
            nullptr);

        // The scheduler's own infrastructure stays eager.
        BOOST_TEST(
            ioc.template find_service<detail::timer_service>() != nullptr);
    }

    void testObjectCreatesOnlyItsOwnService()
    {
        io_context ioc(Backend);

        tcp_socket sock(ioc);
        BOOST_TEST(ioc.template find_service<tcp_service_t>() != nullptr);
        BOOST_TEST(ioc.template find_service<udp_service_t>() == nullptr);
        BOOST_TEST(
            ioc.template find_service<tcp_acceptor_service_t>() == nullptr);
    }

    void testEachObjectCreatesItsService()
    {
        io_context ioc(Backend);

        tcp_acceptor acc(ioc);
        BOOST_TEST(
            ioc.template find_service<tcp_acceptor_service_t>() != nullptr);

        udp_socket us(ioc);
        BOOST_TEST(ioc.template find_service<udp_service_t>() != nullptr);

        local_stream_socket ls(ioc);
        BOOST_TEST(
            ioc.template find_service<local_stream_service_t>() != nullptr);

        local_stream_acceptor la(ioc);
        BOOST_TEST(
            ioc.template find_service<local_stream_acceptor_service_t>() !=
            nullptr);

#if !BOOST_COROSIO_HAS_IOCP
        local_datagram_socket ld(ioc);
        BOOST_TEST(
            ioc.template find_service<local_datagram_service_t>() != nullptr);

        posix_descriptor pd(ioc);
        BOOST_TEST(
            ioc.template find_service<descriptor_service_t>() != nullptr);
#endif

        resolver res(ioc);
        BOOST_TEST(ioc.template find_service<resolver_service_t>() != nullptr);

        signal_set sigs(ioc);
        BOOST_TEST(ioc.template find_service<signal_service_t>() != nullptr);

        stream_file sf(ioc);
        BOOST_TEST(ioc.template find_service<file_service_t>() != nullptr);

        random_access_file raf(ioc);
        BOOST_TEST(
            ioc.template find_service<random_access_file_service_t>() !=
            nullptr);
    }

    void testConcurrentFirstUse()
    {
        // Get-or-create must be safe when the first sockets are
        // constructed from multiple threads at once.
        io_context ioc(Backend);

        std::vector<std::thread> threads;
        std::vector<std::unique_ptr<tcp_socket>> socks(8);
        for (std::size_t i = 0; i < socks.size(); ++i)
            threads.emplace_back([&, i] {
                socks[i] = std::make_unique<tcp_socket>(ioc);
            });
        for (auto& t : threads)
            t.join();

        BOOST_TEST(ioc.template find_service<tcp_service_t>() != nullptr);
        for (auto& s : socks)
            BOOST_TEST(s != nullptr);
    }

    void run()
    {
        testConstructionCreatesNoIoServices();
        testObjectCreatesOnlyItsOwnService();
        testEachObjectCreatesItsService();
        testConcurrentFirstUse();
    }
};

COROSIO_BACKEND_TESTS(lazy_services_test, "boost.corosio.lazy_services")

} // namespace boost::corosio
