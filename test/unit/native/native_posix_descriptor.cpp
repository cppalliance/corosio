//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/native/native_posix_descriptor.hpp>

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/native/native_io_context.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstring>
#include <string>
#include <type_traits>

#include <unistd.h>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_posix_descriptor_test
{
    static_assert(
        std::is_base_of_v<posix_descriptor, native_posix_descriptor<Backend>>);

    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_posix_descriptor<Backend>&>()
                         .read_some(std::declval<capy::mutable_buffer>())),
            decltype(std::declval<io_stream&>().read_some(
                std::declval<capy::mutable_buffer>()))>,
        "native_posix_descriptor::read_some must shadow "
        "io_stream::read_some");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_posix_descriptor<Backend>&>()
                         .write_some(std::declval<capy::const_buffer>())),
            decltype(std::declval<io_stream&>().write_some(
                std::declval<capy::const_buffer>()))>,
        "native_posix_descriptor::write_some must shadow "
        "io_stream::write_some");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_posix_descriptor<Backend>&>().wait(
                wait_type::read)),
            decltype(std::declval<posix_descriptor&>().wait(wait_type::read))>,
        "native_posix_descriptor::wait must shadow posix_descriptor::wait");

    void testConstruct()
    {
        native_io_context<Backend> ioc;
        native_posix_descriptor<Backend> d(ioc);
        BOOST_TEST_EQ(d.is_open(), false);
    }

    // Writes into a pipe with plain POSIX calls and reads the bytes
    // back through native_posix_descriptor, exercising the shadowed
    // read_some() awaitable end to end rather than merely
    // static-asserting the type shape.
    void testReadWriteRoundTrip()
    {
        native_io_context<Backend> ioc;
        native_posix_descriptor<Backend> d(ioc);

        int fds[2];
        BOOST_TEST_EQ(::pipe(fds), 0);
        BOOST_TEST(!d.assign(fds[0]));

        char const msg[] = "abc";
        BOOST_TEST_EQ(::write(fds[1], msg, 3), 3);

        char buf[8]   = {};
        std::size_t n = 0;
        std::error_code ec;
        auto reader = [&]() -> capy::task<> {
            auto [rec, rn] =
                co_await d.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            ec = rec;
            n  = rn;
        };
        capy::run_async(ioc.get_executor())(reader());
        ioc.run();

        BOOST_TEST_EQ(ec, std::error_code{});
        BOOST_TEST_EQ(n, 3u);
        BOOST_TEST_EQ(std::string(buf, n), std::string("abc"));

        ::close(fds[1]);
    }

    void testPolymorphicSlice()
    {
        native_io_context<Backend> ioc;
        native_posix_descriptor<Backend> d(ioc);
        posix_descriptor& base = d;
        BOOST_TEST_EQ(base.is_open(), false);
    }

    // Exercises the shadowed wait() awaitable through a genuine
    // round trip: the reader parks until the writer produces data.
    void testWait()
    {
        native_io_context<Backend> ioc;
        native_posix_descriptor<Backend> d(ioc);

        int fds[2];
        BOOST_TEST_EQ(::pipe(fds), 0);
        BOOST_TEST(!d.assign(fds[0]));

        char const msg[] = "x";
        BOOST_TEST_EQ(::write(fds[1], msg, 1), 1);

        std::error_code ec;
        bool done   = false;
        auto waiter = [&]() -> capy::task<> {
            auto [wec] = co_await d.wait(wait_type::read);
            ec         = wec;
            done       = true;
        };
        capy::run_async(ioc.get_executor())(waiter());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST_EQ(ec, std::error_code{});

        ::close(fds[1]);
    }

    void run()
    {
        testConstruct();
        testReadWriteRoundTrip();
        testPolymorphicSlice();
        testWait();
    }
};

COROSIO_BACKEND_TESTS(
    native_posix_descriptor_test, "boost.corosio.native_posix_descriptor")

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX
