//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/native/native_stream_file.hpp>

#include <boost/corosio/native/native_io_context.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <type_traits>
#include <utility>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

namespace {

struct temp_file
{
    std::filesystem::path path;

    explicit temp_file(std::string_view prefix = "corosio_native_sf_")
    {
        // Per-process random_device-seeded RNG so concurrent test
        // processes (e.g. ctest --parallel running .epoll and .select
        // variants of the same suite) don't collide on identical paths.
        static thread_local std::mt19937_64 gen{std::random_device{}()};
        path = std::filesystem::temp_directory_path()
             / (std::string(prefix) + std::to_string(gen()));
    }

    temp_file(std::string_view prefix, std::string_view contents)
        : temp_file(prefix)
    {
        std::ofstream ofs(path, std::ios::binary);
        ofs.write(
            contents.data(),
            static_cast<std::streamsize>(contents.size()));
    }

    ~temp_file()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    temp_file(temp_file const&)            = delete;
    temp_file& operator=(temp_file const&) = delete;
};

} // namespace

template<auto Backend>
struct native_stream_file_test
{
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_stream_file<Backend>&>().read_some(
                std::declval<capy::mutable_buffer>())),
            decltype(std::declval<io_stream&>().read_some(
                std::declval<capy::mutable_buffer>()))>,
        "native_stream_file::read_some must shadow io_stream::read_some");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_stream_file<Backend>&>().write_some(
                std::declval<capy::const_buffer>())),
            decltype(std::declval<io_stream&>().write_some(
                std::declval<capy::const_buffer>()))>,
        "native_stream_file::write_some must shadow io_stream::write_some");

    void testConstruct()
    {
        io_context ioc(Backend);
        native_stream_file<Backend> f(ioc);
        BOOST_TEST_EQ(f.is_open(), false);
    }

    void testPolymorphicSlice()
    {
        io_context ioc(Backend);
        temp_file tmp("native_sf_slice_", "x");
        native_stream_file<Backend> f(ioc);
        f.open(tmp.path, file_base::read_only);

        stream_file& base = f;
        BOOST_TEST(base.is_open());
    }

    void testReadSome()
    {
        std::string data = "hello native";
        temp_file tmp("native_sf_read_", data);

        io_context ioc(Backend);
        native_stream_file<Backend> f(ioc);
        f.open(tmp.path, file_base::read_only);

        char buf[64] = {};
        std::size_t n_out = 0;

        auto task = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await f.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST_EQ(ec, std::error_code{});
            n_out = n;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();

        BOOST_TEST_EQ(n_out, data.size());
        BOOST_TEST_EQ(std::memcmp(buf, data.data(), data.size()), 0);
    }

    void testWriteSome()
    {
        temp_file tmp("native_sf_write_");

        io_context ioc(Backend);
        native_stream_file<Backend> f(ioc);
        f.open(
            tmp.path,
            file_base::write_only | file_base::create | file_base::truncate);

        char const msg[] = "native write";
        std::size_t written = 0;

        auto task = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await f.write_some(capy::const_buffer(msg, sizeof(msg) - 1));
            BOOST_TEST_EQ(ec, std::error_code{});
            written = n;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();

        BOOST_TEST_EQ(written, sizeof(msg) - 1);

        f.close();
        std::ifstream ifs(tmp.path, std::ios::binary);
        std::string contents(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        BOOST_TEST_EQ(contents, std::string(msg));
    }

    void testVirtualDispatchFallback()
    {
        std::string data = "fallback";
        temp_file tmp("native_sf_fb_", data);

        io_context ioc(Backend);
        native_stream_file<Backend> f(ioc);
        f.open(tmp.path, file_base::read_only);

        stream_file& base = f;

        char buf[64] = {};
        std::size_t n_out = 0;
        auto task = [&]() -> capy::task<> {
            auto [ec, n] = co_await base.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST_EQ(ec, std::error_code{});
            n_out = n;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();

        BOOST_TEST_EQ(n_out, data.size());
    }

    void run()
    {
        testConstruct();
        testPolymorphicSlice();
        testReadSome();
        testWriteSome();
        testVirtualDispatchFallback();
    }
};

COROSIO_BACKEND_TESTS(
    native_stream_file_test, "boost.corosio.native.stream_file")

} // namespace boost::corosio
