//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4s.native-descriptors.adoc.

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
#if defined(_MSC_VER)
#pragma warning(disable : 4834) // discarding [[nodiscard]] return value
#pragma warning(disable : 4189) // local variable initialized but not referenced
#pragma warning(disable : 4100) // unreferenced formal parameter
#pragma warning(disable : 4101) // unreferenced local variable
#endif

#include <boost/corosio/detail/platform.hpp>

#include "test_suite.hpp"

// The page documents a POSIX-only type, so the whole body is guarded --
// including the `assume` fragment, which names <unistd.h>.
#if BOOST_COROSIO_POSIX

// tag::assume[]
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/posix_descriptor.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>

#include <cerrno>
#include <system_error>

#include <unistd.h>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;
// end::assume[]

#include <boost/capy/ex/run_async.hpp>

#include <cstdint>
#include <cstring>

#if defined(__linux__)
#include <sys/eventfd.h>
#include <sys/inotify.h>
#endif

namespace {

std::error_code
last_error() noexcept
{
    return std::error_code(errno, std::system_category());
}

// tag::layering[]
// Nothing below is descriptor-specific. `capy::read` is constrained on
// capy::Stream, and a posix_descriptor models it exactly as a
// tcp_socket or a tls_stream does, so the same algorithm drives all
// three.
capy::task<std::error_code>
fill(corosio::posix_descriptor& d, capy::mutable_buffer buf)
{
    auto [ec, n] = co_await capy::read(d, buf);
    co_return ec;
}
// end::layering[]

#if defined(__linux__)

capy::task<std::error_code>
count_events(corosio::io_context& ioc)
{
    // tag::adopt_eventfd[]
    int fd = ::eventfd(0, EFD_CLOEXEC);
    if (fd < 0)
        co_return last_error();

    corosio::posix_descriptor d(ioc);
    if (auto ec = d.assign(fd))
    {
        // A failed assign() leaves the descriptor with the caller.
        ::close(fd);
        co_return ec;
    }

    // An eventfd delivers its accumulated count as a single 8-byte
    // host-order integer; the read parks until the count is nonzero
    // and resets it to zero.
    std::uint64_t count = 0;
    auto [ec, n] =
        co_await d.read_some(capy::mutable_buffer(&count, sizeof(count)));
    // end::adopt_eventfd[]
    co_return ec;
}

capy::task<std::error_code>
watch_directory(corosio::io_context& ioc, char const* path)
{
    // tag::adopt_inotify[]
    int fd = ::inotify_init1(IN_CLOEXEC);
    if (fd < 0)
        co_return last_error();
    if (::inotify_add_watch(fd, path, IN_CREATE | IN_DELETE) < 0)
    {
        auto ec = last_error();
        ::close(fd);
        co_return ec;
    }

    corosio::posix_descriptor d(ioc);
    if (auto ec = d.assign(fd))
    {
        ::close(fd);
        co_return ec;
    }

    // inotify delivers whole events. The buffer must be aligned for
    // inotify_event and large enough for at least one event plus its
    // variable-length name.
    alignas(struct inotify_event) char buf[4096];
    auto [ec, n] = co_await d.read_some(capy::mutable_buffer(buf, sizeof(buf)));
    if (ec)
        co_return ec;

    auto const* ev = reinterpret_cast<struct inotify_event const*>(buf);
    // end::adopt_inotify[]
    (void)ev;
    co_return ec;
}

#endif // __linux__

capy::task<std::error_code>
read_a_line(corosio::io_context& ioc)
{
    // tag::wait_only[]
    // wait() transfers no bytes and sets no flag, so standard input --
    // and the terminal the parent shell shares with it -- stays exactly
    // as the process inherited it.
    int fd = ::dup(STDIN_FILENO);
    if (fd < 0)
        co_return last_error();

    corosio::posix_descriptor d(ioc);
    if (auto ec = d.assign(fd))
    {
        ::close(fd);
        co_return ec;
    }

    auto [ec] = co_await d.wait(corosio::wait_type::read);
    if (ec)
        co_return ec;

    // Readable, so a blocking ::read returns here. Readiness is not a
    // general guarantee against parking -- a socket can report ready
    // and still have nothing to hand over -- but it holds for a tty.
    char line[256];
    auto n = ::read(d.native_handle(), line, sizeof(line));
    // end::wait_only[]
    if (n < 0)
        co_return last_error();
    co_return std::error_code{};
}

// Stand-in for a C library that owns a descriptor and does its own I/O
// on it (the libpq shape).
struct foreign_conn
{};

int
foreign_fd(foreign_conn*) noexcept
{
    return -1;
}

capy::task<std::error_code>
watch_foreign(corosio::io_context& ioc, foreign_conn* conn)
{
    // tag::dup_for_foreign_fd[]
    // Adopt a duplicate, never the library's own descriptor. Both refer
    // to one open file description, so readiness is identical and the
    // lazily applied O_NONBLOCK is visible to the library either way --
    // but corosio's close() can only ever close the copy.
    int copy = ::dup(foreign_fd(conn));
    if (copy < 0)
        co_return last_error();

    corosio::posix_descriptor d(ioc);
    if (auto ec = d.assign(copy))
    {
        ::close(copy);
        co_return ec;
    }
    // end::dup_for_foreign_fd[]

    auto [ec] = co_await d.wait(corosio::wait_type::read);
    co_return ec;
}

capy::task<>
run_and_store(capy::task<std::error_code> t, std::error_code& ec_out)
{
    ec_out = co_await std::move(t);
}

struct native_descriptors_test
{
    // Adopt the read end of a pipe and drive it through the generic
    // stream algorithm: the claim the page makes about layering is the
    // one worth testing for real.
    void testLayering()
    {
        int fds[2];
        BOOST_TEST(::pipe(fds) == 0);

        corosio::io_context ioc;
        corosio::posix_descriptor d(ioc);
        BOOST_TEST(!d.assign(fds[0]));

        char got[5] = {};
        std::error_code ec = std::make_error_code(std::errc::io_error);
        capy::run_async(ioc.get_executor())(
            run_and_store(fill(d, capy::mutable_buffer(got, sizeof(got))), ec));

        // All five bytes are in the pipe before the read issues, so
        // this checks the adopt -> capy::read -> read_some path end to
        // end, not the algorithm's short-read branch.
        BOOST_TEST(::write(fds[1], "hello", 5) == 5);

        ioc.run();
        BOOST_TEST(!ec);
        BOOST_TEST(std::memcmp(got, "hello", 5) == 0);
        ::close(fds[1]);
    }

    void run()
    {
        testLayering();
    }
};

} // namespace

#else

namespace {
struct native_descriptors_test
{
    void run() {}
};
} // namespace

#endif // BOOST_COROSIO_POSIX

TEST_SUITE(native_descriptors_test, "boost.corosio.doc.4s_native_descriptors");
