//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_UNIT_WIN_TEST_HANDLES_HPP
#define BOOST_COROSIO_TEST_UNIT_WIN_TEST_HANDLES_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>

namespace boost::corosio::test {

/// Closes the handle it holds; `release()` hands ownership away.
class unique_handle
{
public:
    unique_handle() = default;
    explicit unique_handle(HANDLE h) noexcept : h_(h) {}
    ~unique_handle() { reset(); }

    unique_handle(unique_handle&& o) noexcept : h_(o.release()) {}
    unique_handle& operator=(unique_handle&& o) noexcept
    {
        if (this != &o)
        {
            reset();
            h_ = o.release();
        }
        return *this;
    }

    HANDLE get() const noexcept { return h_; }

    HANDLE release() noexcept
    {
        HANDLE h = h_;
        h_       = nullptr;
        return h;
    }

    void reset() noexcept
    {
        if (h_ && h_ != INVALID_HANDLE_VALUE)
            ::CloseHandle(h_);
        h_ = nullptr;
    }

    explicit operator bool() const noexcept
    {
        return h_ && h_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE h_ = nullptr;
};

inline native_handle_type
as_native(HANDLE h) noexcept
{
    return reinterpret_cast<native_handle_type>(h);
}

// ctest runs the .iocp suites of different files concurrently, so
// every name carries the pid and a per-process counter.
inline std::wstring
unique_suffix()
{
    static std::atomic<unsigned> counter{0};
    return std::to_wstring(::GetCurrentProcessId()) + L"_" +
        std::to_wstring(counter++);
}

/// A connected named pipe: `server` is overlapped (the end tests
/// adopt), `client` is synchronous (the end tests drive directly).
struct pipe_pair
{
    unique_handle server;
    unique_handle client;
};

inline pipe_pair
make_pipe_pair(bool message_mode = false)
{
    auto const name = L"\\\\.\\pipe\\corosio_test_" + unique_suffix();
    DWORD const type = message_mode
        ? (PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE)
        : (PIPE_TYPE_BYTE | PIPE_READMODE_BYTE);

    pipe_pair p;
    p.server = unique_handle(::CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
        type | PIPE_WAIT, 1, 4096, 4096, 0, nullptr));
    // Opening the client connects the instance; no ConnectNamedPipe.
    p.client = unique_handle(::CreateFileW(
        name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, 0, nullptr));
    if (message_mode && p.client)
    {
        DWORD mode = PIPE_READMODE_MESSAGE;
        ::SetNamedPipeHandleState(p.client.get(), &mode, nullptr, nullptr);
    }
    return p;
}

/// Like make_pipe_pair, but the client end is overlapped (the end tests
/// adopt) and the server end is driven directly by the test.
inline pipe_pair
make_pipe_pair_overlapped_client()
{
    auto const name = L"\\\\.\\pipe\\corosio_test_" + unique_suffix();
    pipe_pair p;
    p.server = unique_handle(::CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0,
        nullptr));
    p.client = unique_handle(::CreateFileW(
        name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    return p;
}

/// A temp-directory path removed on destruction.
struct temp_path
{
    std::filesystem::path path;

    explicit temp_path(char const* tag)
        : path(
              std::filesystem::temp_directory_path() /
              (L"corosio_" + std::filesystem::path(tag).wstring() + L"_" +
               unique_suffix()))
    {
    }

    ~temp_path()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

inline unique_handle
open_file(std::filesystem::path const& p, bool overlapped)
{
    return unique_handle(::CreateFileW(
        p.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | (overlapped ? FILE_FLAG_OVERLAPPED : 0),
        nullptr));
}

inline unique_handle
open_directory(std::filesystem::path const& p)
{
    return unique_handle(::CreateFileW(
        p.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr));
}

/// Synchronous write on a non-overlapped handle (a pipe's client end).
inline bool
write_all(HANDLE h, char const* data, DWORD n)
{
    DWORD written = 0;
    return ::WriteFile(h, data, n, &written, nullptr) && written == n;
}

} // namespace boost::corosio::test

#endif // BOOST_COROSIO_HAS_IOCP

#endif
