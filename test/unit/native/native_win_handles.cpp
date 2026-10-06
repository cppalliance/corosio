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
#include <boost/corosio/native/native_win_object_handle.hpp>
#include <boost/corosio/native/native_win_random_access_handle.hpp>
#include <boost/corosio/native/native_win_stream_handle.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstring>

#include "test_suite.hpp"
#include "win_test_handles.hpp"

namespace boost::corosio {

static_assert(std::is_base_of_v<win_stream_handle, native_win_stream_handle<iocp>>);
static_assert(std::is_base_of_v<
              win_random_access_handle,
              native_win_random_access_handle<iocp>>);
static_assert(std::is_base_of_v<
              win_object_handle,
              native_win_object_handle<iocp>>);

struct native_win_handles_test
{
    void testStreamHandleRoundTrip()
    {
        io_context ioc(iocp);
        native_win_stream_handle<iocp> h(ioc);
        auto p = test::make_pipe_pair();
        BOOST_TEST(!h.assign(test::as_native(p.server.release())));
        BOOST_TEST(test::write_all(p.client.get(), "twin", 4));

        std::error_code ec;
        std::size_t n = 0;
        char buf[8]{};
        auto task = [&]() -> capy::task<> {
            auto [e, rn] =
                co_await h.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            ec = e;
            n  = rn;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 4u);
        BOOST_TEST(std::memcmp(buf, "twin", 4) == 0);
    }

    void testRandomAccessHandleRoundTrip()
    {
        test::temp_path t("twin_rah");
        io_context ioc(iocp);
        native_win_random_access_handle<iocp> h(ioc);
        auto f = test::open_file(t.path, true);
        BOOST_TEST(!h.assign(test::as_native(f.release())));
        std::error_code ec;
        std::size_t n = 0;
        auto task = [&]() -> capy::task<> {
            auto [e, wn] = co_await h.write_some_at(0, capy::const_buffer("rt", 2));
            ec = e;
            n  = wn;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 2u);
    }

    void testObjectHandleWait()
    {
        io_context ioc(iocp);
        native_win_object_handle<iocp> o(ioc);
        HANDLE ev = ::CreateEventW(nullptr, TRUE, TRUE, nullptr);
        BOOST_TEST(!o.assign(test::as_native(ev)));
        std::error_code ec = std::make_error_code(std::errc::io_error);
        auto task = [&]() -> capy::task<> {
            auto [e] = co_await o.wait();
            ec = e;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(!ec);
    }

    void run()
    {
        testStreamHandleRoundTrip();
        testRandomAccessHandleRoundTrip();
        testObjectHandleWait();
    }
};

TEST_SUITE(native_win_handles_test, "boost.corosio.native_win_handles");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP
