//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

// Test that header file is self-contained.
#include <boost/corosio/native/detail/iocp/win_overlapped_handle.hpp>

#include "win_test_handles.hpp"
#include "test_suite.hpp"

namespace boost::corosio::detail {

struct win_handle_error_map_test
{
    void testReadEndOfDataBecomesZero()
    {
        // Zero with zero bytes decodes as eof in decode_io_result.
        BOOST_TEST_EQ(normalize_handle_error(ERROR_HANDLE_EOF, true), 0u);
        BOOST_TEST_EQ(normalize_handle_error(ERROR_BROKEN_PIPE, true), 0u);
    }

    void testReadMoreDataIsPartialSuccess()
    {
        BOOST_TEST_EQ(normalize_handle_error(ERROR_MORE_DATA, true), 0u);
    }

    void testWriteCodesPassThrough()
    {
        BOOST_TEST_EQ(
            normalize_handle_error(ERROR_BROKEN_PIPE, false),
            DWORD(ERROR_BROKEN_PIPE));
        BOOST_TEST_EQ(
            normalize_handle_error(ERROR_MORE_DATA, false),
            DWORD(ERROR_MORE_DATA));
        BOOST_TEST_EQ(
            normalize_handle_error(ERROR_ACCESS_DENIED, true),
            DWORD(ERROR_ACCESS_DENIED));
    }

    void testBrokenPipeCodes()
    {
        BOOST_TEST(
            iocp_make_err(ERROR_BROKEN_PIPE, false) == std::errc::broken_pipe);
        BOOST_TEST(
            iocp_make_err(ERROR_NO_DATA, false) == std::errc::broken_pipe);
        BOOST_TEST(
            iocp_make_err(ERROR_PIPE_NOT_CONNECTED, false) ==
            std::errc::broken_pipe);
    }

    void testServerDisconnectReadsAsEof()
    {
        BOOST_TEST_EQ(
            normalize_handle_error(ERROR_PIPE_NOT_CONNECTED, true), 0u);
    }

    void testDissociateHandleAllowsRebinding()
    {
        auto p = test::make_pipe_pair();
        test::unique_handle port1(
            ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
        test::unique_handle port2(
            ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
        BOOST_TEST(::CreateIoCompletionPort(p.server.get(), port1.get(), 0, 0));
        // Bound handles cannot join a second port...
        BOOST_TEST(!::CreateIoCompletionPort(p.server.get(), port2.get(), 0, 0));
        // ...until dissociated.
        BOOST_TEST(dissociate_from_iocp(p.server.get()));
        BOOST_TEST(::CreateIoCompletionPort(p.server.get(), port2.get(), 0, 0));
    }

    void run()
    {
        testReadEndOfDataBecomesZero();
        testReadMoreDataIsPartialSuccess();
        testWriteCodesPassThrough();
        testBrokenPipeCodes();
        testServerDisconnectReadsAsEof();
        testDissociateHandleAllowsRebinding();
    }
};

TEST_SUITE(win_handle_error_map_test, "boost.corosio.win_handle_error_map");

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP
