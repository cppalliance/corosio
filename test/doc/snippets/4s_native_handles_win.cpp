//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in the "Windows Handles" section of
// pages/4.guide/4s.native-descriptors.adoc.

#include "../doc_warnings.hpp"

#include <boost/corosio/detail/platform.hpp>

#include "test_suite.hpp"

// The section documents IOCP-only types, so the whole body is guarded --
// including the `assume_win` fragment, which names <windows.h>.
#if BOOST_COROSIO_HAS_IOCP

// tag::assume_win[]
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/timeout.hpp>
#include <boost/corosio/win_object_handle.hpp>
#include <boost/corosio/win_random_access_handle.hpp>
#include <boost/corosio/win_stream_handle.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/when_all.hpp>

#include <chrono>
#include <string>
#include <system_error>

#include <windows.h>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

// Adopting a HANDLE converts it to the library's handle type.
inline corosio::native_handle_type
native(HANDLE h) noexcept
{
    return reinterpret_cast<corosio::native_handle_type>(h);
}
// end::assume_win[]

#include <boost/capy/ex/run_async.hpp>

#include <atomic>
#include <filesystem>

namespace {

std::error_code
last_error() noexcept
{
    return std::error_code(
        static_cast<int>(::GetLastError()), std::system_category());
}

// tag::overlapped_pipe[]
// A stand-in for CreatePipe whose read end is overlapped. The write end
// can stay synchronous. To pass it to a child process, open it
// inheritable: SECURITY_ATTRIBUTES with bInheritHandle = TRUE.
std::error_code
make_overlapped_pipe(HANDLE& read_end, HANDLE& write_end)
{
    // Pipe names are global; make this one unique to the process.
    static std::atomic<unsigned> counter{0};
    std::wstring name = L"\\\\.\\pipe\\myapp_" +
        std::to_wstring(::GetCurrentProcessId()) + L"_" +
        std::to_wstring(counter++);

    read_end = ::CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, nullptr);
    if (read_end == INVALID_HANDLE_VALUE)
        return last_error();

    // Opening the client end connects the pipe; no ConnectNamedPipe.
    write_end = ::CreateFileW(
        name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (write_end == INVALID_HANDLE_VALUE)
    {
        auto ec = last_error();
        ::CloseHandle(read_end);
        return ec;
    }
    return {};
}

// Read everything the writer sends, until it closes its end.
capy::task<std::error_code>
read_all(corosio::io_context& ioc, HANDLE read_end, std::string& out)
{
    corosio::win_stream_handle p(ioc);
    if (auto ec = p.assign(native(read_end)))
    {
        // A failed assign() leaves the handle with the caller.
        ::CloseHandle(read_end);
        co_return ec;
    }

    char buf[4096];
    for (;;)
    {
        auto [ec, n] =
            co_await capy::read(p, capy::mutable_buffer(buf, sizeof(buf)));
        out.append(buf, n);
        if (ec == capy::cond::eof)
            co_return {}; // the writer closed its end
        if (ec)
            co_return ec;
    }
}
// end::overlapped_pipe[]

std::error_code
reject_anonymous_pipe(corosio::io_context& ioc)
{
    // tag::rejected_handles[]
    HANDLE r = nullptr;
    HANDLE w = nullptr;
    if (!::CreatePipe(&r, &w, nullptr, 0))
        return last_error();

    // Both ends of an anonymous pipe are synchronous, so assign()
    // fails with errc::operation_not_supported. The handles stay yours.
    corosio::win_stream_handle p(ioc);
    auto ec = p.assign(native(r));
    if (ec == std::errc::operation_not_supported)
    {
        ::CloseHandle(r);
        ::CloseHandle(w);
    }

    // The overlapped named pipe is the substitute.
    if (auto err = make_overlapped_pipe(r, w))
        return err;
    auto named = p.assign(native(r)); // succeeds; p now owns the read end
    if (named)
        ::CloseHandle(r); // a failed assign() leaves the handle with you
    ::CloseHandle(w);
    // end::rejected_handles[]
    return named ? named : ec;
}

capy::task<std::error_code>
write_two_records(corosio::io_context& ioc, wchar_t const* path)
{
    // tag::random_access[]
    HANDLE h = ::CreateFileW(
        path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        co_return last_error();

    corosio::win_random_access_handle f(ioc);
    if (auto ec = f.assign(native(h)))
    {
        ::CloseHandle(h);
        co_return ec;
    }

    // Each write names its own offset, so both can be in flight at once.
    char const a[] = "first record";
    char const b[] = "second record";
    auto [ec, na, nb] = co_await capy::when_all(
        f.write_some_at(0, capy::const_buffer(a, sizeof(a))),
        f.write_some_at(4096, capy::const_buffer(b, sizeof(b))));
    // end::random_access[]
    co_return ec;
}

capy::task<std::error_code>
run_child(corosio::io_context& ioc, DWORD& exit_code)
{
    // tag::wait_for_process[]
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    wchar_t cmd[] = L"cmd.exe /c exit 3"; // CreateProcessW may modify it
    if (!::CreateProcessW(
            nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            nullptr, &si, &pi))
        co_return last_error();
    ::CloseHandle(pi.hThread);

    corosio::win_object_handle child(ioc);
    if (auto ec = child.assign(native(pi.hProcess)))
    {
        ::CloseHandle(pi.hProcess);
        co_return ec;
    }

    // A process handle becomes signaled when the process exits.
    auto [ec] = co_await child.wait();
    if (ec)
        co_return ec;

    ::GetExitCodeProcess(
        reinterpret_cast<HANDLE>(child.native_handle()), &exit_code);
    // end::wait_for_process[]
    co_return std::error_code{};
}

capy::task<std::error_code>
wait_briefly(corosio::io_context& ioc, HANDLE event)
{
    // tag::wait_with_timeout[]
    corosio::win_object_handle o(ioc);
    if (auto ec = o.assign(native(event)))
    {
        ::CloseHandle(event);
        co_return ec;
    }

    // wait() takes no timeout; compose it with corosio::timeout.
    auto [ec] = co_await corosio::timeout(
        o.wait(), std::chrono::milliseconds(500));

    if (ec == capy::cond::timeout)
    {
        // Not signaled within 500ms. The timed-out wait consumed
        // nothing; give up on the object and close its handle.
        o.close();
    }
    // end::wait_with_timeout[]
    co_return ec;
}

capy::task<>
run_and_store(capy::task<std::error_code> t, std::error_code& ec_out)
{
    ec_out = co_await std::move(t);
}

struct native_handles_win_test
{
    void testOverlappedPipe()
    {
        HANDLE r = nullptr;
        HANDLE w = nullptr;
        BOOST_TEST(!make_overlapped_pipe(r, w));

        DWORD written = 0;
        BOOST_TEST(::WriteFile(w, "hello", 5, &written, nullptr));
        BOOST_TEST(written == 5u);
        ::CloseHandle(w);

        corosio::io_context ioc;
        std::string out;
        std::error_code ec = std::make_error_code(std::errc::io_error);
        capy::run_async(ioc.get_executor())(
            run_and_store(read_all(ioc, r, out), ec));
        ioc.run();
        BOOST_TEST(!ec);
        BOOST_TEST(out == "hello");
    }

    void testRejectedHandles()
    {
        corosio::io_context ioc;
        BOOST_TEST(
            reject_anonymous_pipe(ioc) == std::errc::operation_not_supported);
    }

    void testRandomAccess()
    {
        auto path = std::filesystem::temp_directory_path() /
            (L"corosio_doc_4s_" + std::to_wstring(::GetCurrentProcessId()));

        {
            corosio::io_context ioc;
            std::error_code ec = std::make_error_code(std::errc::io_error);
            capy::run_async(ioc.get_executor())(
                run_and_store(write_two_records(ioc, path.c_str()), ec));
            ioc.run();
            BOOST_TEST(!ec);
        }

        std::error_code ec;
        BOOST_TEST(std::filesystem::file_size(path, ec) == 4096u + 14u);
        std::filesystem::remove(path, ec);
    }

    void testWaitForProcess()
    {
        corosio::io_context ioc;
        DWORD code = 0;
        std::error_code ec = std::make_error_code(std::errc::io_error);
        capy::run_async(ioc.get_executor())(
            run_and_store(run_child(ioc, code), ec));
        ioc.run();
        BOOST_TEST(!ec);
        BOOST_TEST(code == 3u);
    }

    void testWaitWithTimeout()
    {
        // A manual-reset event nobody sets.
        HANDLE e = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOST_TEST(e != nullptr);

        corosio::io_context ioc;
        std::error_code ec;
        capy::run_async(ioc.get_executor())(
            run_and_store(wait_briefly(ioc, e), ec));
        ioc.run();
        BOOST_TEST(ec == capy::cond::timeout);
    }

    void run()
    {
        testOverlappedPipe();
        testRejectedHandles();
        testRandomAccess();
        testWaitForProcess();
        testWaitWithTimeout();
    }
};

} // namespace

#else

namespace {
struct native_handles_win_test
{
    void run() {}
};
} // namespace

#endif // BOOST_COROSIO_HAS_IOCP

TEST_SUITE(native_handles_win_test, "boost.corosio.doc.4s_native_handles_win");
