//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4o.file-io.adoc.

// Fragments deliberately leave results and bindings unused; the pages
// explain the values in prose instead.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-lambda-capture"
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4834) // discarding [[nodiscard]] return value
#pragma warning(disable: 4189) // local variable initialized but not referenced
#pragma warning(disable: 4100) // unreferenced formal parameter
#pragma warning(disable: 4101) // unreferenced local variable
#pragma warning(disable: 4456) // declaration hides previous local declaration
#pragma warning(disable: 4457) // declaration hides function parameter
#pragma warning(disable: 4458) // declaration hides class member
#pragma warning(disable: 4459) // declaration hides global declaration
#endif

// tag::assume[]
#include <boost/corosio/stream_file.hpp>
#include <boost/corosio/random_access_file.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::assume[]

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/test/temp_path.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#if BOOST_COROSIO_POSIX
#include <fcntl.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <tuple>

#include "test_suite.hpp"

namespace {

capy::task<>
stream_read(
    corosio::io_context& ioc, bool& eof_seen, std::size_t& bytes_read)
{
    // tag::stream_read[]
    corosio::stream_file f(ioc);
    if (auto ec = f.open("data.bin", corosio::file_base::read_only))
        co_return;  // open failed

    char buf[4096];
    auto [ec, n] = co_await f.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));

    if (ec == capy::cond::eof)
        // reached end of file
    // end::stream_read[]
        eof_seen = true;
    bytes_read = n;
}

capy::task<>
stream_write(
    corosio::io_context& ioc, std::error_code& ec_out, std::size_t& n_out)
{
    // tag::stream_write[]
    corosio::stream_file f(ioc);
    if (auto ec = f.open("output.bin",
            corosio::file_base::write_only
            | corosio::file_base::create
            | corosio::file_base::truncate))
        co_return;  // open failed

    std::string data = "hello world";
    auto [ec, n] = co_await f.write_some(
        capy::const_buffer(data.data(), data.size()));
    // end::stream_write[]
    ec_out = ec;
    n_out = n;
}

std::uint64_t
reposition(corosio::stream_file& f)
{
    // tag::seek[]
    auto [ec, pos] = f.seek(0, corosio::file_base::seek_set);  // beginning
    if (! ec)
        std::tie(ec, pos) =
            f.seek(100, corosio::file_base::seek_cur);  // forward 100 bytes
    if (! ec)
        std::tie(ec, pos) =
            f.seek(-10, corosio::file_base::seek_end);  // 10 before end
    // end::seek[]
    BOOST_TEST(!ec);
    return pos;
}

capy::task<>
read_at(
    corosio::io_context& ioc, std::error_code& ec_out,
    std::size_t& n_out, char& first)
{
    // tag::read_at[]
    corosio::random_access_file f(ioc);
    if (auto ec = f.open("data.bin", corosio::file_base::read_only))
        co_return;  // open failed

    char buf[256];
    auto [ec, n] = co_await f.read_some_at(
        1024,  // byte offset
        capy::mutable_buffer(buf, sizeof(buf)));
    // end::read_at[]
    ec_out = ec;
    n_out = n;
    first = buf[0];
}

capy::task<>
write_at(
    corosio::io_context& ioc, std::error_code& ec_out, std::size_t& n_out)
{
    // tag::write_at[]
    corosio::random_access_file f(ioc);
    if (auto ec = f.open("data.bin", corosio::file_base::read_write))
        co_return;  // open failed

    auto [ec, n] = co_await f.write_some_at(
        512, capy::const_buffer("patched", 7));
    // end::write_at[]
    ec_out = ec;
    n_out = n;
}

void
open_log(corosio::stream_file& f)
{
    // tag::open_flags[]
    if (auto ec = f.open("log.txt",
            corosio::file_base::write_only
            | corosio::file_base::create
            | corosio::file_base::append))
        return;  // report the error
    // end::open_flags[]
}

void
inspect_metadata(corosio::stream_file& f)
{
    // tag::metadata[]
    auto bytes = f.size();                 // file size in bytes
    if (auto ec = f.resize(1024))          // truncate or extend
        return;
    if (auto ec = f.sync_data())           // flush data to stable storage
        return;
    if (auto ec = f.sync_all())            // flush data and metadata
        return;
    // end::metadata[]
}

// Opens and closes handles through the platform API directly: assign
// requires a handle Corosio has never registered (see the page NOTE).
corosio::native_handle_type
open_platform_handle(char const* path)
{
#if BOOST_COROSIO_POSIX
    return ::open(path, O_RDONLY);
#else
    return reinterpret_cast<corosio::native_handle_type>(
        ::CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
#endif
}

void
close_platform_handle(
    corosio::native_handle_type h)
{
#if BOOST_COROSIO_POSIX
    ::close(h);
#else
    ::CloseHandle(reinterpret_cast<HANDLE>(h));
#endif
}

void
release_handle(corosio::random_access_file& f)
{
    // tag::native_release[]
    // Release ownership — caller must close the handle
    auto handle = f.release();
    assert(!f.is_open());
    // end::native_release[]
    close_platform_handle(handle);
}

void
adopt_handle(
    corosio::io_context& ioc,
    corosio::native_handle_type native_handle,
    bool& adopted)
{
    // tag::native_adopt[]
    // Adopt a handle obtained from the platform's file API —
    // the file object takes ownership
    corosio::random_access_file f2(ioc);
    auto ec = f2.assign(native_handle);
    // end::native_adopt[]
    adopted = !ec && f2.is_open();
    f2.close();
}

capy::task<>
read_at_eof(
    corosio::stream_file& f, capy::mutable_buffer buf,
    std::error_code& ec_out)
{
    // tag::error_handling[]
    auto [ec, n] = co_await f.read_some(buf);
    if (ec == capy::cond::eof)
    {
        // no more data
    }
    else if (ec)
    {
        // I/O error
    }
    // end::error_handling[]
    ec_out = ec;
}

struct file_io_test
{
    void
    testStreamRead()
    {
        corosio::io_context ioc;
        bool eof_seen = false;
        std::size_t n = 0;
        capy::run_async(ioc.get_executor())(stream_read(ioc, eof_seen, n));
        ioc.run();
        BOOST_TEST(!eof_seen);
        BOOST_TEST_EQ(n, 2048u);
    }

    void
    testStreamWrite()
    {
        corosio::io_context ioc;
        std::error_code ec;
        std::size_t n = 0;
        capy::run_async(ioc.get_executor())(stream_write(ioc, ec, n));
        ioc.run();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 11u);

        std::ifstream in("output.bin", std::ios::binary);
        std::string contents(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
        BOOST_TEST(contents == "hello world");
    }

    void
    testSeek()
    {
        corosio::io_context ioc;
        corosio::stream_file f(ioc);
        BOOST_TEST(!f.open("data.bin", corosio::file_base::read_only));
        // 2048-byte file: 10 bytes before end is position 2038
        BOOST_TEST_EQ(reposition(f), 2038u);
    }

    void
    testReadAt()
    {
        corosio::io_context ioc;
        std::error_code ec;
        std::size_t n = 0;
        char first = 0;
        capy::run_async(ioc.get_executor())(read_at(ioc, ec, n, first));
        ioc.run();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 256u);
        BOOST_TEST_EQ(first, 'a');
    }

    void
    testWriteAt()
    {
        corosio::io_context ioc;
        std::error_code ec;
        std::size_t n = 0;
        capy::run_async(ioc.get_executor())(write_at(ioc, ec, n));
        ioc.run();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 7u);

        std::ifstream in("data.bin", std::ios::binary);
        in.seekg(512);
        char patched[8] = {};
        in.read(patched, 7);
        BOOST_TEST(std::string_view(patched, 7) == "patched");
    }

    void
    testOpenFlagsAndMetadata()
    {
        corosio::io_context ioc;
        corosio::stream_file f(ioc);
        open_log(f);
        BOOST_TEST(f.is_open());
        inspect_metadata(f);
        BOOST_TEST_EQ(f.size(), 1024u);
    }

    void
    testNativeHandle()
    {
        corosio::io_context ioc;
        corosio::random_access_file f(ioc);
        BOOST_TEST(!f.open("data.bin", corosio::file_base::read_only));
        release_handle(f);

        bool adopted = false;
        adopt_handle(ioc, open_platform_handle("data.bin"), adopted);
        BOOST_TEST(adopted);
    }

    void
    testErrorHandling()
    {
        corosio::io_context ioc;
        corosio::stream_file f(ioc);
        BOOST_TEST(!f.open("data.bin", corosio::file_base::read_only));
        auto [sec, spos] = f.seek(0, corosio::file_base::seek_end);
        BOOST_TEST(!sec);
        char buf[64];
        std::error_code ec;
        capy::run_async(ioc.get_executor())(read_at_eof(
            f, capy::mutable_buffer(buf, sizeof(buf)), ec));
        ioc.run();
        BOOST_TEST(ec == capy::cond::eof);
    }

    void
    run()
    {
        namespace fs = std::filesystem;

        // The page fragments open files by relative name; run them
        // inside a private temp directory so parallel test processes
        // cannot collide and cleanup is automatic.
        struct cwd_guard
        {
            fs::path old = fs::current_path();
            ~cwd_guard()
            {
                std::error_code ec;
                fs::current_path(old, ec);
            }
        };
        cwd_guard guard;
        boost::corosio::test::temp_socket_dir dir;
        fs::current_path(fs::path(dir.path()).parent_path());

        {
            std::ofstream out("data.bin", std::ios::binary);
            std::string filler(2048, 'a');
            out.write(filler.data(),
                static_cast<std::streamsize>(filler.size()));
        }

        testStreamRead();
        testStreamWrite();
        testSeek();
        testReadAt();
        testWriteAt();
        testOpenFlagsAndMetadata();
        testNativeHandle();
        testErrorHandling();
    }
};

} // namespace

TEST_SUITE(file_io_test, "boost.corosio.doc.4o_file_io");
