//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/native/detail/iocp/win_windows.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>

#include <system_error>

#include "test_suite.hpp"

namespace boost::corosio {

struct iocp_error_map_test
{
    // Both the WSA and Win32 form of each code must land on the same
    // portable condition, and ERROR_NETNAME_DELETED must split by
    // operation (reset for stream I/O, aborted for accept).
    void run()
    {
        using detail::iocp_make_err;

        BOOST_TEST(
            iocp_make_err(ERROR_NETNAME_DELETED, /*accept_path=*/false) ==
            std::errc::connection_reset);
        BOOST_TEST(
            iocp_make_err(ERROR_NETNAME_DELETED, /*accept_path=*/true) ==
            std::errc::connection_aborted);

        BOOST_TEST(
            iocp_make_err(WSAECONNRESET, false) ==
            std::errc::connection_reset);
        BOOST_TEST(
            iocp_make_err(WSAECONNREFUSED, false) ==
            std::errc::connection_refused);
        BOOST_TEST(
            iocp_make_err(ERROR_CONNECTION_REFUSED, false) ==
            std::errc::connection_refused);
        BOOST_TEST(
            iocp_make_err(WSAECONNABORTED, false) ==
            std::errc::connection_aborted);
        BOOST_TEST(
            iocp_make_err(ERROR_CONNECTION_ABORTED, false) ==
            std::errc::connection_aborted);
        BOOST_TEST(
            iocp_make_err(WSAENETUNREACH, false) ==
            std::errc::network_unreachable);
        BOOST_TEST(
            iocp_make_err(ERROR_NETWORK_UNREACHABLE, false) ==
            std::errc::network_unreachable);
        BOOST_TEST(
            iocp_make_err(WSAEHOSTUNREACH, false) ==
            std::errc::host_unreachable);
        BOOST_TEST(
            iocp_make_err(ERROR_HOST_UNREACHABLE, false) ==
            std::errc::host_unreachable);
        BOOST_TEST(
            iocp_make_err(WSAETIMEDOUT, false) == std::errc::timed_out);
        BOOST_TEST(
            iocp_make_err(ERROR_SEM_TIMEOUT, false) == std::errc::timed_out);

        // Unmapped codes defer to make_err and stay non-empty.
        BOOST_TEST(!!iocp_make_err(ERROR_ACCESS_DENIED, false));
    }
};

TEST_SUITE(iocp_error_map_test, "boost.corosio.iocp_error_map");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP
