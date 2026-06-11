//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/random_access_file.hpp>

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
#include <random>
#include <atomic>
#include <limits>
#include <stop_token>
#include <string>
#include <system_error>

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX
#include <fcntl.h>
#include <unistd.h>
#else
#include <boost/corosio/native/detail/iocp/win_windows.hpp>
#endif

namespace boost::corosio {

namespace {

// Unique across concurrently running per-backend test processes;
// unseeded std::rand() yields the same sequence in every process.
inline std::string
unique_path_suffix()
{
    static unsigned const seed = std::random_device{}();
    static std::atomic<unsigned> counter{0};
    return std::to_string(seed) + "_"
        + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

struct temp_file
{
    std::filesystem::path path;

    temp_file(std::string_view prefix = "corosio_raf_test_")
    {
        path = std::filesystem::temp_directory_path()
             / (std::string(prefix) + unique_path_suffix());
    }

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
struct random_access_file_test
{
    // Construction

    void testConstruction()
    {
        io_context ioc(Backend);
        random_access_file f(ioc);

        BOOST_TEST(!f.is_open());
        BOOST_TEST_PASS();
    }

    void testConstructionFromExecutor()
    {
        io_context ioc(Backend);
        random_access_file f(ioc.get_executor());

        BOOST_TEST(!f.is_open());
        BOOST_TEST_PASS();
    }

    void testMoveConstruct()
    {
        io_context ioc(Backend);
        random_access_file f1(ioc);
        random_access_file f2(std::move(f1));

        BOOST_TEST_PASS();
    }

    // Open / close

    void testOpenReadOnly()
    {
        temp_file tmp("raf_open_ro_", "hello");
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path, file_base::read_only);
        BOOST_TEST(f.is_open());

        f.close();
        BOOST_TEST(!f.is_open());
    }

    void testOpenNonexistent()
    {
        io_context ioc(Backend);
        random_access_file f(ioc);

        bool threw = false;
        try
        {
            f.open("/tmp/corosio_nonexistent_raf_zzz_12345",
                   file_base::read_only);
        }
        catch (std::system_error const&)
        {
            threw = true;
        }
        BOOST_TEST(threw);
    }

    // File metadata

    void testSize()
    {
        std::string data = "0123456789";
        temp_file tmp("raf_size_", data);
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path, file_base::read_only);
        BOOST_TEST_EQ(f.size(), static_cast<std::uint64_t>(data.size()));
    }

    void testResize()
    {
        temp_file tmp("raf_resize_", "0123456789");
        io_context ioc(Backend);
        random_access_file f(ioc);

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

    // Async read at offset

    void testReadSomeAt()
    {
        std::string data = "ABCDEFGHIJ";
        temp_file tmp("raf_read_", data);
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path, file_base::read_only);

        bool completed = false;
        char buf[5] = {};

        auto task = [](random_access_file& f_ref, char* buf_ptr,
                       bool& done) -> capy::task<> {
            // Read 5 bytes starting at offset 3
            auto [ec, n] = co_await f_ref.read_some_at(
                3, capy::mutable_buffer(buf_ptr, 5));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, 5u);
            done = true;
        };
        capy::run_async(ioc.get_executor())(task(f, buf, completed));

        ioc.run();

        BOOST_TEST(completed);
        BOOST_TEST(std::memcmp(buf, "DEFGH", 5) == 0);
    }

    void testReadSomeAtBeginning()
    {
        std::string data = "hello world";
        temp_file tmp("raf_read0_", data);
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path, file_base::read_only);

        bool completed = false;
        char buf[5] = {};

        auto task = [](random_access_file& f_ref, char* buf_ptr,
                       bool& done) -> capy::task<> {
            auto [ec, n] = co_await f_ref.read_some_at(
                0, capy::mutable_buffer(buf_ptr, 5));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, 5u);
            done = true;
        };
        capy::run_async(ioc.get_executor())(task(f, buf, completed));

        ioc.run();

        BOOST_TEST(completed);
        BOOST_TEST(std::memcmp(buf, "hello", 5) == 0);
    }

    void testReadSomeAtEOF()
    {
        temp_file tmp("raf_eof_", "hi");
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path, file_base::read_only);

        bool got_eof = false;

        auto task = [](random_access_file& f_ref,
                       bool& eof_out) -> capy::task<> {
            char buf[64];
            // Read past end of file
            auto [ec, n] = co_await f_ref.read_some_at(
                100, capy::mutable_buffer(buf, sizeof(buf)));
            eof_out = (ec == capy::cond::eof);
        };
        capy::run_async(ioc.get_executor())(task(f, got_eof));

        ioc.run();

        BOOST_TEST(got_eof);
    }

    // Async write at offset

    void testWriteSomeAt()
    {
        temp_file tmp("raf_write_");
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path,
               file_base::read_write | file_base::create | file_base::truncate);

        bool completed = false;

        auto task = [](random_access_file& f_ref, bool& done) -> capy::task<> {
            // Write "hello" at offset 0
            auto [ec, n] = co_await f_ref.write_some_at(
                0, capy::const_buffer("hello", 5));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, 5u);
            done = true;
        };
        capy::run_async(ioc.get_executor())(task(f, completed));

        ioc.run();

        BOOST_TEST(completed);

        // Verify by reading back
        f.close();
        std::ifstream ifs(tmp.path, std::ios::binary);
        std::string contents(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        BOOST_TEST_EQ(contents, "hello");
    }

    void testWriteAndReadAtDifferentOffsets()
    {
        temp_file tmp("raf_wroff_");
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path,
               file_base::read_write | file_base::create | file_base::truncate);

        bool completed = false;

        auto task = [](random_access_file& f_ref, bool& done) -> capy::task<> {
            // Write "AAA" at offset 0
            {
                auto [ec, n] = co_await f_ref.write_some_at(
                    0, capy::const_buffer("AAA", 3));
                BOOST_TEST(!ec);
                BOOST_TEST_EQ(n, 3u);
            }

            // Write "BBB" at offset 3
            {
                auto [ec, n] = co_await f_ref.write_some_at(
                    3, capy::const_buffer("BBB", 3));
                BOOST_TEST(!ec);
                BOOST_TEST_EQ(n, 3u);
            }

            // Read back from offset 0
            char buf[6] = {};
            {
                auto [ec, n] = co_await f_ref.read_some_at(
                    0, capy::mutable_buffer(buf, 6));
                BOOST_TEST(!ec);
                BOOST_TEST_EQ(n, 6u);
            }
            BOOST_TEST(std::memcmp(buf, "AAABBB", 6) == 0);

            // Read back from offset 2 (crossing the boundary)
            char buf2[4] = {};
            {
                auto [ec, n] = co_await f_ref.read_some_at(
                    2, capy::mutable_buffer(buf2, 4));
                BOOST_TEST(!ec);
                BOOST_TEST_EQ(n, 4u);
            }
            BOOST_TEST(std::memcmp(buf2, "ABBB", 4) == 0);

            done = true;
        };
        capy::run_async(ioc.get_executor())(task(f, completed));

        ioc.run();

        BOOST_TEST(completed);
    }

    // Sequential operations

    void testSequentialReads()
    {
        std::string data = "0123456789ABCDEF";
        temp_file tmp("raf_seqrd_", data);
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path, file_base::read_only);

        int read_count = 0;

        auto task = [](random_access_file& f_ref,
                       int& count_out) -> capy::task<> {
            char buf[4];

            for (std::uint64_t i = 0; i < 4; ++i)
            {
                auto [ec, n] = co_await f_ref.read_some_at(
                    i * 4, capy::mutable_buffer(buf, 4));
                BOOST_TEST(!ec);
                BOOST_TEST_EQ(n, 4u);
                ++count_out;
            }
        };
        capy::run_async(ioc.get_executor())(task(f, read_count));

        ioc.run();

        BOOST_TEST_EQ(read_count, 4);
    }

    // Cancel

    void testCancelNoOperation()
    {
        temp_file tmp("raf_cancel_", "data");
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path, file_base::read_only);
        f.cancel();

        BOOST_TEST_PASS();
    }

    void testCancelOnClosedFile()
    {
        io_context ioc(Backend);
        random_access_file f(ioc);

        // cancel() on a closed file is a no-op (early return).
        f.cancel();
        BOOST_TEST(!f.is_open());
    }

    void testNativeHandleClosedAndOpen()
    {
        temp_file tmp("raf_nh_", "x");
        io_context ioc(Backend);
        random_access_file f(ioc);

#if BOOST_COROSIO_HAS_IOCP
        auto const invalid = static_cast<native_handle_type>(~0ull);
#else
        auto const invalid = static_cast<native_handle_type>(-1);
#endif
        BOOST_TEST(f.native_handle() == invalid);

        f.open(tmp.path, file_base::read_only);
        BOOST_TEST(f.native_handle() != invalid);
    }

    void testOpenReplacesExisting()
    {
        temp_file tmp1("raf_replace_a_", "first");
        temp_file tmp2("raf_replace_b_", "second");
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp1.path, file_base::read_only);
        BOOST_TEST(f.is_open());

        // Reopen on an already-open file closes the previous handle.
        f.open(tmp2.path, file_base::read_only);
        BOOST_TEST(f.is_open());
    }

    // Sync data

    void testSyncData()
    {
        temp_file tmp("raf_sync_");
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path,
               file_base::write_only | file_base::create | file_base::truncate);

        bool completed = false;

        auto task = [](random_access_file& f_ref,
                       bool& done) -> capy::task<> {
            auto [ec, n] = co_await f_ref.write_some_at(
                0, capy::const_buffer("sync", 4));
            BOOST_TEST(!ec);
            f_ref.sync_data();
            done = true;
        };
        capy::run_async(ioc.get_executor())(task(f, completed));

        ioc.run();

        BOOST_TEST(completed);
    }

    // Concurrent operations

    void testConcurrentReads()
    {
        // 4 coroutines reading different offsets of the same file
        std::string data = "AAAABBBBCCCCDDDD";
        temp_file tmp("raf_conc_rd_", data);
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path, file_base::read_only);

        int completed = 0;

        auto reader = [](random_access_file& f_ref, std::uint64_t off,
                         char expected, int& count) -> capy::task<> {
            char buf[4] = {};
            auto [ec, n] = co_await f_ref.read_some_at(
                off, capy::mutable_buffer(buf, 4));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, 4u);
            for (int i = 0; i < 4; ++i)
                BOOST_TEST_EQ(buf[i], expected);
            ++count;
        };

        // Launch 4 concurrent readers on the same file
        capy::run_async(ioc.get_executor())(reader(f, 0, 'A', completed));
        capy::run_async(ioc.get_executor())(reader(f, 4, 'B', completed));
        capy::run_async(ioc.get_executor())(reader(f, 8, 'C', completed));
        capy::run_async(ioc.get_executor())(reader(f, 12, 'D', completed));

        ioc.run();

        BOOST_TEST_EQ(completed, 4);
    }

    void testConcurrentWrites()
    {
        // 4 coroutines writing non-overlapping offsets
        temp_file tmp("raf_conc_wr_");
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path,
               file_base::read_write | file_base::create | file_base::truncate);
        f.resize(16);

        int completed = 0;

        auto writer = [](random_access_file& f_ref, std::uint64_t off,
                         char ch, int& count) -> capy::task<> {
            char buf[4];
            std::memset(buf, ch, 4);
            auto [ec, n] = co_await f_ref.write_some_at(
                off, capy::const_buffer(buf, 4));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, 4u);
            ++count;
        };

        capy::run_async(ioc.get_executor())(writer(f, 0, 'W', completed));
        capy::run_async(ioc.get_executor())(writer(f, 4, 'X', completed));
        capy::run_async(ioc.get_executor())(writer(f, 8, 'Y', completed));
        capy::run_async(ioc.get_executor())(writer(f, 12, 'Z', completed));

        ioc.run();

        BOOST_TEST_EQ(completed, 4);

        // Verify file contents
        f.close();
        std::ifstream ifs(tmp.path, std::ios::binary);
        std::string contents(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        BOOST_TEST_EQ(contents, "WWWWXXXXYYYYZZZZ");
    }

    void testConcurrentReadWrite()
    {
        // Simultaneous read and write at different offsets
        std::string data = "0123456789ABCDEF";
        temp_file tmp("raf_conc_rw_", data);
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path, file_base::read_write);

        bool read_done  = false;
        bool write_done = false;

        auto reader = [](random_access_file& f_ref,
                         bool& done) -> capy::task<> {
            char buf[4] = {};
            auto [ec, n] = co_await f_ref.read_some_at(
                0, capy::mutable_buffer(buf, 4));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, 4u);
            done = true;
        };

        auto writer = [](random_access_file& f_ref,
                         bool& done) -> capy::task<> {
            auto [ec, n] = co_await f_ref.write_some_at(
                12, capy::const_buffer("ZZZZ", 4));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, 4u);
            done = true;
        };

        capy::run_async(ioc.get_executor())(reader(f, read_done));
        capy::run_async(ioc.get_executor())(writer(f, write_done));

        ioc.run();

        BOOST_TEST(read_done);
        BOOST_TEST(write_done);
    }

    void testManyConcurrentOps()
    {
        // Stress test: 100 concurrent reads
        constexpr std::size_t num_ops = 100;
        constexpr std::size_t block_sz = 4;
        std::string data(num_ops * block_sz, 'X');
        for (std::size_t i = 0; i < num_ops; ++i)
            std::memset(data.data() + i * block_sz,
                        'A' + (i % 26), block_sz);

        temp_file tmp("raf_many_", data);
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path, file_base::read_only);

        std::atomic<int> completed{0};

        auto reader = [](random_access_file& f_ref, std::uint64_t off,
                         char expected, std::atomic<int>& count) -> capy::task<> {
            char buf[block_sz] = {};
            auto [ec, n] = co_await f_ref.read_some_at(
                off, capy::mutable_buffer(buf, block_sz));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, static_cast<std::size_t>(block_sz));
            for (std::size_t i = 0; i < block_sz; ++i)
                BOOST_TEST_EQ(buf[i], expected);
            ++count;
        };

        for (std::size_t i = 0; i < num_ops; ++i)
        {
            capy::run_async(ioc.get_executor())(
                reader(f, i * block_sz,
                       static_cast<char>('A' + (i % 26)), completed));
        }

        ioc.run();

        BOOST_TEST_EQ(completed.load(), num_ops);
    }

    void run()
    {
        testConstruction();
        testConstructionFromExecutor();
        testMoveConstruct();

        testOpenReadOnly();
        testOpenNonexistent();

        testSize();
        testResize();

        testReadSomeAt();
        testReadSomeAtBeginning();
        testReadSomeAtEOF();

        testWriteSomeAt();
        testWriteAndReadAtDifferentOffsets();

        testSequentialReads();

        testCancelNoOperation();
        testCancelOnClosedFile();
        testNativeHandleClosedAndOpen();
        testOpenReplacesExisting();
        testSyncData();

        testConcurrentReads();
        testConcurrentWrites();
        testConcurrentReadWrite();
        testManyConcurrentOps();

        testSyncAll();
        testRelease();
        testAssign();
        testClosedFileThrows();
        testOpenSyncAllOnWrite();
        testOpenExclusiveExistingFails();
        testOpenExclusiveNewFile();
        testEmptyBufferReadWrite();
        testReadAtPastEofErrorPath();
        testCancelInflightOperation();
        testCancelWithStoppedToken();
    }

    // Operations on closed file

    void testClosedFileThrows()
    {
        io_context ioc(Backend);
        random_access_file f(ioc);
        BOOST_TEST(!f.is_open());

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
    }

    // Open flag variants

    void testOpenSyncAllOnWrite()
    {
        // Exercises the O_SYNC mapping in posix_random_access_file::open_file.
        temp_file tmp("raf_sync_open_");
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path,
               file_base::write_only | file_base::create
                   | file_base::truncate | file_base::sync_all_on_write);
        BOOST_TEST(f.is_open());

        bool done = false;
        auto task = [](random_access_file& f_ref,
                       bool& d) -> capy::task<> {
            auto [ec, n] = co_await f_ref.write_some_at(
                0, capy::const_buffer("synced", 6));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, 6u);
            d = true;
        };
        capy::run_async(ioc.get_executor())(task(f, done));
        ioc.run();
        BOOST_TEST(done);
    }

    void testOpenExclusiveExistingFails()
    {
        // create|exclusive on an existing file maps to O_EXCL and
        // surfaces EEXIST.
        temp_file tmp("raf_excl_", "x");
        io_context ioc(Backend);
        random_access_file f(ioc);

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
        BOOST_TEST(!f.is_open());
    }

    void testOpenExclusiveNewFile()
    {
        // create|exclusive on a new file path succeeds and exercises
        // the O_EXCL flag mapping.
        temp_file tmp("raf_excl_new_"); // no contents, file does not exist
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path,
               file_base::write_only | file_base::create
                   | file_base::exclusive);
        BOOST_TEST(f.is_open());
        f.close();
    }

    void testEmptyBufferReadWrite()
    {
        // Zero-byte read/write short-circuits before the pool dispatch
        // (early return at the top of read_some_at/write_some_at).
        temp_file tmp("raf_empty_", "hi");
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path, file_base::read_write);

        bool done = false;
        auto task = [](random_access_file& f_ref, bool& d) -> capy::task<> {
            auto [rec, rn] = co_await f_ref.read_some_at(
                0, capy::mutable_buffer(nullptr, 0));
            BOOST_TEST(!rec);
            BOOST_TEST_EQ(rn, 0u);

            auto [wec, wn] = co_await f_ref.write_some_at(
                0, capy::const_buffer(nullptr, 0));
            BOOST_TEST(!wec);
            BOOST_TEST_EQ(wn, 0u);
            d = true;
        };
        capy::run_async(ioc.get_executor())(task(f, done));
        ioc.run();
        BOOST_TEST(done);
    }

    void testCancelInflightOperation()
    {
        // Launch many concurrent reads, then call cancel() to mark the
        // outstanding_ops_ list. Exercises the for_each callback that
        // stamps each op's cancelled flag.
        std::string data(std::size_t{64} * 1024, 'X');
        temp_file tmp("raf_cancel_inflight_", data);
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path, file_base::read_only);

        constexpr std::uint64_t num_ops = 16;
        std::atomic<int> completed{0};

        auto reader = [](random_access_file* f, std::uint64_t off,
                         std::atomic<int>* c) -> capy::task<> {
            char buf[1024];
            auto [ec, n] =
                co_await f->read_some_at(off, capy::mutable_buffer(buf, 1024));
            (void)ec;
            (void)n;
            c->fetch_add(1);
        };

        for (std::uint64_t i = 0; i < num_ops; ++i)
            capy::run_async(ioc.get_executor())(reader(&f, i * 1024, &completed));

        // Immediately cancel before any op can complete.
        f.cancel();

        ioc.run();

        BOOST_TEST_EQ(completed.load(), num_ops);
    }

    void testReadAtPastEofErrorPath()
    {
        // Reading past end returns eof error code via the op-completion
        // bytes_transferred==0 branch (different from explicit EOF earlier).
        temp_file tmp("raf_pasteof_", "abc");
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path, file_base::read_only);

        bool done = false;
        auto task = [](random_access_file& f_ref, bool& d) -> capy::task<> {
            char buf[16];
            auto [ec, n] = co_await f_ref.read_some_at(
                1000, capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(ec); // EOF or io error - non-zero size, zero read
            BOOST_TEST_EQ(n, 0u);
            d = true;
        };
        capy::run_async(ioc.get_executor())(task(f, done));
        ioc.run();
        BOOST_TEST(done);
    }

    // Cancellation

    void testCancelWithStoppedToken()
    {
        temp_file tmp("raf_cancel_tok_", "hello world");
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path, file_base::read_only);

        std::stop_source stop_src;
        stop_src.request_stop();

        bool completed = false;
        std::error_code result_ec;

        auto task = [](random_access_file& f_ref,
                       std::error_code& ec_out,
                       bool& done) -> capy::task<> {
            char buf[64];
            auto [ec, n] = co_await f_ref.read_some_at(
                0, capy::mutable_buffer(buf, sizeof(buf)));
            ec_out = ec;
            done   = true;
        };
        capy::run_async(ioc.get_executor(), stop_src.get_token())(
            task(f, result_ec, completed));

        ioc.run();

        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    // sync_all

    void testSyncAll()
    {
        temp_file tmp("raf_syncall_");
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path,
               file_base::write_only | file_base::create | file_base::truncate);

        bool completed = false;

        auto task = [](random_access_file& f_ref,
                       bool& done) -> capy::task<> {
            auto [ec, n] = co_await f_ref.write_some_at(
                0, capy::const_buffer("sync_all", 8));
            BOOST_TEST(!ec);
            f_ref.sync_all();
            done = true;
        };
        capy::run_async(ioc.get_executor())(task(f, completed));

        ioc.run();

        BOOST_TEST(completed);
    }

    // release

    void testRelease()
    {
        temp_file tmp("raf_release_", "hello");
        io_context ioc(Backend);
        random_access_file f(ioc);

        f.open(tmp.path, file_base::read_only);
        BOOST_TEST(f.is_open());

        auto handle = f.release();
        BOOST_TEST(!f.is_open());

        // The handle is still valid — we can read from it
        char buf[5] = {};
#if BOOST_COROSIO_HAS_IOCP
        // The released handle is still IOCP-associated, so we must
        // set the low-order bit of hEvent to prevent the completion
        // from being posted to the (unserviced) IOCP port.
        HANDLE h = reinterpret_cast<HANDLE>(handle);
        HANDLE evt = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
        OVERLAPPED ov{};
        ov.Offset = 0;
        ov.hEvent = reinterpret_cast<HANDLE>(
            reinterpret_cast<ULONG_PTR>(evt) | 1);
        DWORD bytes_read = 0;
        BOOL ok = ::ReadFile(h, buf, 5, &bytes_read, &ov);
        if (!ok && ::GetLastError() == ERROR_IO_PENDING)
            ok = ::GetOverlappedResult(h, &ov, &bytes_read, TRUE);
        BOOST_TEST(ok);
        BOOST_TEST_EQ(bytes_read, 5u);
        ::CloseHandle(evt);
#else
        auto n = ::pread(handle, buf, 5, 0);
        BOOST_TEST_EQ(n, 5);
#endif
        BOOST_TEST(std::memcmp(buf, "hello", 5) == 0);

#if BOOST_COROSIO_HAS_IOCP
        ::CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
        ::close(handle);
#endif
    }

    // assign

    void testAssign()
    {
        temp_file tmp("raf_assign_", "world");

        // Open with raw platform API, then assign to random_access_file
#if BOOST_COROSIO_HAS_IOCP
        HANDLE h = ::CreateFileW(
            tmp.path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr);
        BOOST_TEST(h != INVALID_HANDLE_VALUE);
        auto raw_handle = reinterpret_cast<native_handle_type>(h);
#else
        int fd = ::open(tmp.path.c_str(), O_RDONLY);
        BOOST_TEST(fd >= 0);
        auto raw_handle = fd;
#endif

        io_context ioc(Backend);
        random_access_file f(ioc);
        f.assign(raw_handle);
        BOOST_TEST(f.is_open());

        bool completed = false;

        auto task = [](random_access_file& f_ref,
                       bool& done) -> capy::task<> {
            char buf[5] = {};
            auto [ec, n] = co_await f_ref.read_some_at(
                0, capy::mutable_buffer(buf, 5));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, 5u);
            BOOST_TEST(std::memcmp(buf, "world", 5) == 0);
            done = true;
        };
        capy::run_async(ioc.get_executor())(task(f, completed));

        ioc.run();

        BOOST_TEST(completed);
    }
};

COROSIO_BACKEND_TESTS(random_access_file_test, "boost.corosio.random_access_file")

} // namespace boost::corosio
