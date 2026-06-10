//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/stream_file.hpp>

// GCC emits false-positive "may be used uninitialized" warnings
// for structured bindings with co_await expressions
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include <boost/corosio/io_context.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include "context.hpp"
#include "test_suite.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stop_token>
#include <string>
#include <system_error>
#include <type_traits>

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX
#include <fcntl.h>
#include <unistd.h>
#else
#include <boost/corosio/native/detail/iocp/win_windows.hpp>
#endif

namespace boost::corosio {

namespace {

// RAII helper that creates a temp file and removes it on destruction.
struct temp_file
{
    std::filesystem::path path;

    temp_file(std::string_view prefix = "corosio_test_")
    {
        path = std::filesystem::temp_directory_path()
             / (std::string(prefix) + std::to_string(std::rand()));
    }

    // Create with initial contents
    temp_file(std::string_view prefix, std::string_view contents)
        : temp_file(prefix)
    {
        std::ofstream ofs(path, std::ios::binary);
        ofs.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    ~temp_file()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    temp_file(temp_file const&) = delete;
    temp_file& operator=(temp_file const&) = delete;
};

} // namespace

template<auto Backend>
struct stream_file_test
{
    // Construction and move semantics

    void testConstruction()
    {
        io_context ioc(Backend);
        stream_file f(ioc);

        BOOST_TEST(!f.is_open());
        BOOST_TEST_PASS();
    }

    void testConstructionFromExecutor()
    {
        io_context ioc(Backend);
        stream_file f(ioc.get_executor());

        BOOST_TEST(!f.is_open());
        BOOST_TEST_PASS();
    }

    void testMoveConstruct()
    {
        io_context ioc(Backend);
        stream_file f1(ioc);
        stream_file f2(std::move(f1));

        BOOST_TEST_PASS();
    }

    void testMoveAssign()
    {
        io_context ioc(Backend);
        stream_file f1(ioc);
        stream_file f2(ioc);

        f2 = std::move(f1);
        BOOST_TEST_PASS();
    }

    // Open / close

    void testOpenReadOnly()
    {
        temp_file tmp("sf_open_ro_", "hello");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path, file_base::read_only);
        BOOST_TEST(f.is_open());

        f.close();
        BOOST_TEST(!f.is_open());
    }

    void testOpenCreateWrite()
    {
        temp_file tmp("sf_open_cw_");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path, file_base::write_only | file_base::create);
        BOOST_TEST(f.is_open());
        f.close();

        // File should exist
        BOOST_TEST(std::filesystem::exists(tmp.path));
    }

    void testOpenNonexistent()
    {
        io_context ioc(Backend);
        stream_file f(ioc);

        bool threw = false;
        try
        {
            f.open("/tmp/corosio_nonexistent_file_zzz_12345",
                   file_base::read_only);
        }
        catch (std::system_error const&)
        {
            threw = true;
        }
        BOOST_TEST(threw);
        BOOST_TEST(!f.is_open());
    }

    void testOpenExclusive()
    {
        temp_file tmp("sf_excl_", "existing");
        io_context ioc(Backend);
        stream_file f(ioc);

        // Opening with create|exclusive on an existing file should fail
        bool threw = false;
        try
        {
            f.open(tmp.path,
                   file_base::write_only | file_base::create
                       | file_base::exclusive);
        }
        catch (std::system_error const&)
        {
            threw = true;
        }
        BOOST_TEST(threw);
    }

    void testOpenSyncAllOnWrite()
    {
        // Exercises the O_SYNC mapping in posix_stream_file::open_file.
        temp_file tmp("sf_sync_open_");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path,
               file_base::write_only | file_base::create
                   | file_base::truncate | file_base::sync_all_on_write);
        BOOST_TEST(f.is_open());

        bool done = false;
        auto task = [](stream_file& f_ref, bool& d) -> capy::task<> {
            auto [ec, n] =
                co_await f_ref.write_some(capy::const_buffer("synced", 6));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, 6u);
            d = true;
        };
        capy::run_async(ioc.get_executor())(task(f, done));
        ioc.run();
        BOOST_TEST(done);
    }

    // File metadata

    void testSize()
    {
        std::string data = "hello world";
        temp_file tmp("sf_size_", data);
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path, file_base::read_only);
        BOOST_TEST_EQ(f.size(), static_cast<std::uint64_t>(data.size()));
    }

    void testResize()
    {
        temp_file tmp("sf_resize_", "hello world");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path, file_base::read_write);
        f.resize(5);
        BOOST_TEST_EQ(f.size(), 5u);

#if BOOST_COROSIO_POSIX
        // Larger than off_t can represent: rejected with EOVERFLOW.
        bool caught = false;
        try
        {
            f.resize((std::numeric_limits<std::uint64_t>::max)());
        }
        catch (std::system_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
#endif
    }

    void testSeek()
    {
        temp_file tmp("sf_seek_", "0123456789");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path, file_base::read_only);

        auto pos = f.seek(5, file_base::seek_set);
        BOOST_TEST_EQ(pos, 5u);

        pos = f.seek(3, file_base::seek_cur);
        BOOST_TEST_EQ(pos, 8u);

        pos = f.seek(-2, file_base::seek_end);
        BOOST_TEST_EQ(pos, 8u); // size=10, 10-2=8
    }

    // Async read

    void testReadSome()
    {
        std::string data = "hello world";
        temp_file tmp("sf_read_", data);
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path, file_base::read_only);

        bool completed = false;
        std::error_code result_ec;
        std::size_t result_bytes = 0;
        char buf[64] = {};

        auto task = [](stream_file& f_ref, char* buf_ptr,
                       std::error_code& ec_out, std::size_t& bytes_out,
                       bool& done) -> capy::task<> {
            auto [ec, n] = co_await f_ref.read_some(
                capy::mutable_buffer(buf_ptr, 64));
            ec_out    = ec;
            bytes_out = n;
            done      = true;
        };
        capy::run_async(ioc.get_executor())(
            task(f, buf, result_ec, result_bytes, completed));

        ioc.run();

        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
        BOOST_TEST_EQ(result_bytes, data.size());
        BOOST_TEST(std::memcmp(buf, data.data(), data.size()) == 0);
    }

    void testReadEOF()
    {
        temp_file tmp("sf_eof_", "hi");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path, file_base::read_only);

        bool got_eof = false;

        auto task = [](stream_file& f_ref, bool& eof_out) -> capy::task<> {
            char buf[64];

            // First read: should return 2 bytes
            auto [ec1, n1] = co_await f_ref.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(n1, 2u);

            // Second read: should return EOF
            auto [ec2, n2] = co_await f_ref.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            eof_out = (ec2 == capy::cond::eof);
        };
        capy::run_async(ioc.get_executor())(task(f, got_eof));

        ioc.run();

        BOOST_TEST(got_eof);
    }

    // Async write

    void testWriteSome()
    {
        temp_file tmp("sf_write_");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path,
               file_base::write_only | file_base::create | file_base::truncate);

        std::string data = "written by corosio";
        bool completed = false;

        auto task = [](stream_file& f_ref, std::string const& data_ref,
                       bool& done) -> capy::task<> {
            auto [ec, n] = co_await f_ref.write_some(
                capy::const_buffer(data_ref.data(), data_ref.size()));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, data_ref.size());
            done = true;
        };
        capy::run_async(ioc.get_executor())(task(f, data, completed));

        ioc.run();

        BOOST_TEST(completed);

        // Verify contents by reading the file back
        f.close();
        std::ifstream ifs(tmp.path, std::ios::binary);
        std::string contents(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        BOOST_TEST_EQ(contents, data);
    }

    // Sequential read/write (verifies position tracking)

    void testSequentialReadWrite()
    {
        temp_file tmp("sf_seqrw_");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path,
               file_base::read_write | file_base::create | file_base::truncate);

        bool completed = false;

        auto task = [](stream_file& f_ref, bool& done) -> capy::task<> {
            // Write "AAABBB"
            {
                auto [ec, n] = co_await f_ref.write_some(
                    capy::const_buffer("AAA", 3));
                BOOST_TEST(!ec);
                BOOST_TEST_EQ(n, 3u);
            }
            {
                auto [ec, n] = co_await f_ref.write_some(
                    capy::const_buffer("BBB", 3));
                BOOST_TEST(!ec);
                BOOST_TEST_EQ(n, 3u);
            }

            // Seek back to start
            f_ref.seek(0, file_base::seek_set);

            // Read back
            char buf[6] = {};
            {
                auto [ec, n] = co_await f_ref.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                BOOST_TEST(!ec);
                BOOST_TEST_EQ(n, 6u);
            }
            BOOST_TEST(std::memcmp(buf, "AAABBB", 6) == 0);

            done = true;
        };
        capy::run_async(ioc.get_executor())(task(f, completed));

        ioc.run();

        BOOST_TEST(completed);
    }

    // Sync data

    void testSyncData()
    {
        temp_file tmp("sf_sync_");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path,
               file_base::write_only | file_base::create | file_base::truncate);

        bool completed = false;

        auto task = [](stream_file& f_ref, bool& done) -> capy::task<> {
            auto [ec, n] = co_await f_ref.write_some(
                capy::const_buffer("data", 4));
            BOOST_TEST(!ec);
            f_ref.sync_data();
            done = true;
        };
        capy::run_async(ioc.get_executor())(task(f, completed));

        ioc.run();

        BOOST_TEST(completed);
    }

    // Cancel

    void testCancelNoOperation()
    {
        temp_file tmp("sf_cancel_", "data");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path, file_base::read_only);
        f.cancel(); // Should not crash

        BOOST_TEST_PASS();
    }

    void testCancelOnClosedFile()
    {
        io_context ioc(Backend);
        stream_file f(ioc);

        // cancel() on a closed file is a no-op (early return).
        f.cancel();
        BOOST_TEST(!f.is_open());
    }

    void testNativeHandleClosedAndOpen()
    {
        temp_file tmp("sf_nh_", "x");
        io_context ioc(Backend);
        stream_file f(ioc);

#if BOOST_COROSIO_HAS_IOCP
        auto const invalid = static_cast<native_handle_type>(~0ull);
#else
        auto const invalid = static_cast<native_handle_type>(-1);
#endif
        // Closed: returns the platform sentinel.
        BOOST_TEST(f.native_handle() == invalid);

        f.open(tmp.path, file_base::read_only);
        BOOST_TEST(f.native_handle() != invalid);
    }

    void testOpenReplacesExisting()
    {
        temp_file tmp1("sf_replace_a_", "first");
        temp_file tmp2("sf_replace_b_", "second");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp1.path, file_base::read_only);
        BOOST_TEST(f.is_open());

        // Reopen on an already-open file closes the previous handle.
        f.open(tmp2.path, file_base::read_only);
        BOOST_TEST(f.is_open());
    }

#if BOOST_COROSIO_POSIX
    void testOpenSingleThreadedNotSupported()
    {
#if BOOST_COROSIO_HAS_IO_URING
        // io_uring performs file I/O through the ring itself, so the
        // single-threaded restriction below does not apply.
        if constexpr (std::is_same_v<
                std::remove_const_t<decltype(Backend)>, io_uring_t>)
            return;
#endif
        // POSIX file I/O requires the shared thread pool; in single-threaded
        // mode the service short-circuits with operation_not_supported.
        temp_file tmp("sf_st_", "data");
        io_context ioc(Backend, 1);
        stream_file f(ioc);

        bool caught = false;
        try
        {
            f.open(tmp.path, file_base::read_only);
        }
        catch (std::system_error const& e)
        {
            caught = (e.code() == std::errc::operation_not_supported);
        }
        BOOST_TEST(caught);
    }
#endif

    void testReadEmptyBuffer()
    {
        // Zero-byte read should short-circuit to completion via the
        // empty-iovec fast path in read_some.
        temp_file tmp("sf_read0_", "hello");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path, file_base::read_only);

        bool completed = false;

        auto task = [](stream_file& f_ref, bool& done) -> capy::task<> {
            char b;
            auto [ec, n] = co_await f_ref.read_some(capy::mutable_buffer(&b, 0));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, 0u);
            done = true;
        };
        capy::run_async(ioc.get_executor())(task(f, completed));
        ioc.run();
        BOOST_TEST(completed);
    }

    void testWriteEmptyBuffer()
    {
        // Same empty-iovec fast path for write_some.
        temp_file tmp("sf_write0_");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path,
               file_base::write_only | file_base::create | file_base::truncate);

        bool completed = false;

        auto task = [](stream_file& f_ref, bool& done) -> capy::task<> {
            char const b = 'x';
            auto [ec, n] = co_await f_ref.write_some(capy::const_buffer(&b, 0));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, 0u);
            done = true;
        };
        capy::run_async(ioc.get_executor())(task(f, completed));
        ioc.run();
        BOOST_TEST(completed);
    }

    // Open flags

    void testTruncate()
    {
        temp_file tmp("sf_trunc_", "original data here");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path, file_base::write_only | file_base::truncate);
        BOOST_TEST_EQ(f.size(), 0u);
    }

    // Append mode

    void testAppendMode()
    {
        temp_file tmp("sf_append_", "hello");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path,
               file_base::write_only | file_base::append);

        bool completed = false;

        auto task = [](stream_file& f_ref, bool& done) -> capy::task<> {
            auto [ec, n] = co_await f_ref.write_some(
                capy::const_buffer(" world", 6));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, 6u);
            done = true;
        };
        capy::run_async(ioc.get_executor())(task(f, completed));

        ioc.run();

        BOOST_TEST(completed);

        // Verify write went to end, not beginning
        f.close();
        std::ifstream ifs(tmp.path, std::ios::binary);
        std::string contents(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        BOOST_TEST_EQ(contents, "hello world");
    }

    // sync_all

    void testSyncAll()
    {
        temp_file tmp("sf_syncall_");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path,
               file_base::write_only | file_base::create | file_base::truncate);

        bool completed = false;

        auto task = [](stream_file& f_ref, bool& done) -> capy::task<> {
            auto [ec, n] = co_await f_ref.write_some(
                capy::const_buffer("data", 4));
            BOOST_TEST(!ec);
            f_ref.sync_all();
            done = true;
        };
        capy::run_async(ioc.get_executor())(task(f, completed));

        ioc.run();

        BOOST_TEST(completed);
    }

    // Release and assign

    void testRelease()
    {
        temp_file tmp("sf_release_", "hello");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path, file_base::read_only);
        BOOST_TEST(f.is_open());

        auto handle = f.release();
        BOOST_TEST(!f.is_open());

        // The raw handle should still be usable
        char buf[5] = {};
#if BOOST_COROSIO_HAS_IOCP
        HANDLE h = reinterpret_cast<HANDLE>(handle);
        DWORD bytes_read = 0;
        OVERLAPPED ov{};
        ov.Offset = 0;
        HANDLE evt = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
        ov.hEvent = reinterpret_cast<HANDLE>(
            reinterpret_cast<ULONG_PTR>(evt) | 1);
        BOOL ok = ::ReadFile(h, buf, 5, &bytes_read, &ov);
        if (!ok && ::GetLastError() == ERROR_IO_PENDING)
            ok = ::GetOverlappedResult(h, &ov, &bytes_read, TRUE);
        BOOST_TEST(ok);
        ::CloseHandle(evt);
        ::CloseHandle(h);
#else
        auto n = ::pread(handle, buf, 5, 0);
        BOOST_TEST_EQ(n, 5);
        ::close(handle);
#endif
        BOOST_TEST(std::memcmp(buf, "hello", 5) == 0);
    }

    void testAssign()
    {
        temp_file tmp("sf_assign_", "world");

#if BOOST_COROSIO_HAS_IOCP
        HANDLE h = ::CreateFileW(
            tmp.path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED
                | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        BOOST_TEST(h != INVALID_HANDLE_VALUE);
        auto raw_handle = reinterpret_cast<native_handle_type>(h);
#else
        int fd = ::open(tmp.path.c_str(), O_RDONLY);
        BOOST_TEST(fd >= 0);
        auto raw_handle = fd;
#endif

        io_context ioc(Backend);
        stream_file f(ioc);
        f.assign(raw_handle);
        BOOST_TEST(f.is_open());

        bool completed = false;

        auto task = [](stream_file& f_ref, bool& done) -> capy::task<> {
            char buf[5] = {};
            auto [ec, n] = co_await f_ref.read_some(
                capy::mutable_buffer(buf, 5));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, 5u);
            BOOST_TEST(std::memcmp(buf, "world", 5) == 0);
            done = true;
        };
        capy::run_async(ioc.get_executor())(task(f, completed));

        ioc.run();

        BOOST_TEST(completed);
    }

    // Operations on closed file

    void testClosedFileThrows()
    {
        io_context ioc(Backend);
        stream_file f(ioc);
        BOOST_TEST(!f.is_open());

        // Each operation on a closed file should throw
        auto expect_throw = [](auto fn) {
            bool threw = false;
            try { fn(); }
            catch (std::system_error const&) { threw = true; }
            BOOST_TEST(threw);
        };

        expect_throw([&] { f.size(); });
        expect_throw([&] { f.resize(0); });
        expect_throw([&] { f.sync_data(); });
        expect_throw([&] { f.sync_all(); });
        expect_throw([&] { f.release(); });
        expect_throw([&] { f.seek(0, file_base::seek_set); });
    }

    // Negative seek validation

    void testSeekNegativeThrows()
    {
        temp_file tmp("sf_seekneg_", "0123456789");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path, file_base::read_only);

        // seek_set with negative offset
        bool threw = false;
        try { f.seek(-1, file_base::seek_set); }
        catch (std::system_error const&) { threw = true; }
        BOOST_TEST(threw);

        // seek_end past beginning
        threw = false;
        try { f.seek(-100, file_base::seek_end); }
        catch (std::system_error const&) { threw = true; }
        BOOST_TEST(threw);

        // seek_cur past beginning
        threw = false;
        try { f.seek(-100, file_base::seek_cur); }
        catch (std::system_error const&) { threw = true; }
        BOOST_TEST(threw);
    }

    void run()
    {
        testConstruction();
        testConstructionFromExecutor();
        testMoveConstruct();
        testMoveAssign();

        testOpenReadOnly();
        testOpenCreateWrite();
        testOpenNonexistent();
        testOpenExclusive();
        testOpenSyncAllOnWrite();

        testSize();
        testResize();
        testSeek();

        testReadSome();
        testReadEOF();
        testWriteSome();
        testSequentialReadWrite();

        testSyncData();
        testSyncAll();
        testCancelNoOperation();
        testCancelOnClosedFile();
        testNativeHandleClosedAndOpen();
        testOpenReplacesExisting();
        testReadEmptyBuffer();
        testWriteEmptyBuffer();
#if BOOST_COROSIO_POSIX
        testOpenSingleThreadedNotSupported();
#endif
        testTruncate();

        testAppendMode();
        testRelease();
        testAssign();
        testClosedFileThrows();
        testSeekNegativeThrows();
        testCancelWithStoppedToken();
    }

    // Cancellation

    void testCancelWithStoppedToken()
    {
        temp_file tmp("sf_cancel_tok_", "hello world");
        io_context ioc(Backend);
        stream_file f(ioc);

        f.open(tmp.path, file_base::read_only);

        // Pre-stop the source so the token is already cancelled
        // when the coroutine starts
        std::stop_source stop_src;
        stop_src.request_stop();

        bool completed = false;
        std::error_code result_ec;

        auto task = [](stream_file& f_ref,
                       std::error_code& ec_out,
                       bool& done) -> capy::task<> {
            char buf[64];
            auto [ec, n] = co_await f_ref.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            ec_out = ec;
            done   = true;
        };
        capy::run_async(ioc.get_executor(), stop_src.get_token())(
            task(f, result_ec, completed));

        ioc.run();

        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }
};

COROSIO_BACKEND_TESTS(stream_file_test, "boost.corosio.stream_file")

} // namespace boost::corosio
