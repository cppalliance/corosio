//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_VALIDATE_HANDLE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_VALIDATE_HANDLE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <cstdint>
#include <cstring>
#include <string_view>
#include <system_error>

/* The adopt-time gates for Windows handles -- the counterpart of
   validate_fd.hpp. A gate only inspects the handle. None performs I/O
   or waits on it, so an auto-reset event or a semaphore keeps its
   signal across a gate, accepted or rejected.

   Synchronous-mode handles are rejected by querying the file mode
   rather than by letting CreateIoCompletionPort fail: whether it
   refuses such a handle is undocumented, and an overlapped ReadFile on
   one silently runs to completion inside the call.
*/

namespace boost::corosio::detail {

/// The adopting type; the gates differ per type.
enum class handle_kind
{
    stream_file,
    random_access_file,
    stream_handle,
    random_access_handle
};

namespace win_nt {

using ntstatus = LONG;

struct io_status_block
{
    union
    {
        ntstatus Status;
        void* Pointer;
    };
    ULONG_PTR Information;
};

using query_information_file_fn =
    ntstatus(NTAPI*)(HANDLE, io_status_block*, void*, ULONG, int);
using query_object_fn = ntstatus(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);

inline constexpr int file_mode_information    = 16;
inline constexpr int file_io_completion_notification_information = 41;
inline constexpr int object_basic_information = 0;
inline constexpr int object_type_information  = 2;

inline constexpr ULONG file_synchronous_io_alert    = 0x10;
inline constexpr ULONG file_synchronous_io_nonalert = 0x20;
inline constexpr ULONG file_skip_completion_port_on_success = 0x1;

struct object_basic_info
{
    ULONG Attributes;
    ACCESS_MASK GrantedAccess;
    ULONG HandleCount;
    ULONG PointerCount;
    ULONG Reserved[10];
};

struct unicode_string
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

// The two-step cast through void(*)() is the sanctioned FARPROC
// conversion; a direct cast trips -Wcast-function-type.
template<class Fn>
Fn
ntdll_proc(char const* name) noexcept
{
    if (HMODULE h = ::GetModuleHandleW(L"ntdll.dll"))
        return reinterpret_cast<Fn>(
            reinterpret_cast<void (*)()>(::GetProcAddress(h, name)));
    return nullptr;
}

inline query_information_file_fn
query_information_file() noexcept
{
    static query_information_file_fn const fn =
        ntdll_proc<query_information_file_fn>("NtQueryInformationFile");
    return fn;
}

inline query_object_fn
query_object() noexcept
{
    static query_object_fn const fn =
        ntdll_proc<query_object_fn>("NtQueryObject");
    return fn;
}

} // namespace win_nt

/** Check whether a handle is in skip-completion-port-on-success mode.

    Such a handle queues no packet for an operation that succeeds
    synchronously, so an operation issued through a completion port
    would never complete. The mode cannot be cleared.

    @param h The handle to inspect. Never read, written or waited on.

    @return `true` if the mode is set. A failed query means it never
        was.
*/
inline bool
has_skip_on_success(HANDLE h) noexcept
{
    auto const query = win_nt::query_information_file();
    if (!query)
        return false;
    win_nt::io_status_block iosb{};
    ULONG notify = 0;
    return query(h, &iosb, &notify, sizeof(notify),
               win_nt::file_io_completion_notification_information) >= 0 &&
        (notify & win_nt::file_skip_completion_port_on_success);
}

/** Validate a handle for adoption by an overlapped type.

    @param h The handle to inspect. Never read, written or waited on.
    @param kind The adopting type.

    @return `bad_file_descriptor` for a null, invalid or closed
        handle; `operation_not_supported` for a console, a socket, a
        synchronous-mode handle, a handle already in
        skip-completion-port-on-success mode, a directory, a disk
        handle adopted
        by `win_stream_handle`, or a pipe adopted by a file type;
        otherwise an empty code. A missing `ntdll` entry point fails
        closed with `operation_not_supported`.
*/
inline std::error_code
validate_overlapped_handle(HANDLE h, handle_kind kind) noexcept
{
    auto const not_supported =
        std::make_error_code(std::errc::operation_not_supported);

    if (h == nullptr || h == INVALID_HANDLE_VALUE)
        return std::make_error_code(std::errc::bad_file_descriptor);

    ::SetLastError(NO_ERROR);
    DWORD const type = ::GetFileType(h);
    if (type == FILE_TYPE_UNKNOWN && ::GetLastError() != NO_ERROR)
        return std::make_error_code(std::errc::bad_file_descriptor);

    DWORD console_mode = 0;
    if (type == FILE_TYPE_CHAR && ::GetConsoleMode(h, &console_mode))
        return not_supported;

    auto const query = win_nt::query_information_file();
    if (!query)
        return not_supported;
    win_nt::io_status_block iosb{};
    ULONG mode = 0;
    if (query(h, &iosb, &mode, sizeof(mode), win_nt::file_mode_information) <
        0)
        return not_supported;
    if (mode &
        (win_nt::file_synchronous_io_alert |
         win_nt::file_synchronous_io_nonalert))
        return not_supported;

    if (has_skip_on_success(h))
        return not_supported;

    // A failed query (volumes, some devices) means "not a directory".
    FILE_BASIC_INFO basic{};
    if (::GetFileInformationByHandleEx(
            h, FileBasicInfo, &basic, sizeof(basic)) &&
        (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return not_supported;

    // A SOCKET reports FILE_TYPE_PIPE but is no named pipe, and must be
    // closed with closesocket, not CloseHandle. GetNamedPipeInfo fails
    // on it with ERROR_INVALID_FUNCTION; on a pipe end opened without
    // FILE_READ_ATTRIBUTES (a PIPE_ACCESS_OUTBOUND server) it fails
    // with ERROR_ACCESS_DENIED, which is no reason to reject.
    if (type == FILE_TYPE_PIPE &&
        !::GetNamedPipeInfo(h, nullptr, nullptr, nullptr, nullptr) &&
        ::GetLastError() != ERROR_ACCESS_DENIED)
        return not_supported;

    switch (kind)
    {
    case handle_kind::stream_handle:
        if (type == FILE_TYPE_DISK)
            return not_supported;
        break;
    case handle_kind::stream_file:
    case handle_kind::random_access_file:
        if (type == FILE_TYPE_PIPE)
            return not_supported;
        break;
    case handle_kind::random_access_handle:
        break;
    }
    return {};
}

/** Validate a handle for adoption by `win_object_handle`.

    @return `bad_file_descriptor` for a null, invalid or closed
        handle; `operation_not_supported` for a pseudo-handle, an
        object type other than process, thread, event, semaphore or
        waitable timer, a handle without `SYNCHRONIZE` access, or a
        missing `ntdll` entry point; otherwise an empty code.
*/
inline std::error_code
validate_object_handle(HANDLE h) noexcept
{
    auto const not_supported =
        std::make_error_code(std::errc::operation_not_supported);

    if (h == nullptr || h == INVALID_HANDLE_VALUE)
        return std::make_error_code(std::errc::bad_file_descriptor);

    // GetCurrentThread() and the other pseudo-handles (-2 .. -6) resolve
    // per calling thread. -1 (GetCurrentProcess) is INVALID_HANDLE_VALUE
    // and already rejected above.
    auto const v = reinterpret_cast<std::intptr_t>(h);
    if (v <= -2 && v >= -6)
        return not_supported;

    auto const query = win_nt::query_object();
    if (!query)
        return not_supported;

    win_nt::object_basic_info basic{};
    if (query(
            h, win_nt::object_basic_information, &basic, sizeof(basic),
            nullptr) < 0)
        return std::make_error_code(std::errc::bad_file_descriptor);
    if (!(basic.GrantedAccess & SYNCHRONIZE))
        return not_supported;

    alignas(8) unsigned char buf[1024];
    if (query(h, win_nt::object_type_information, buf, sizeof(buf), nullptr) <
        0)
        return not_supported;
    win_nt::unicode_string name;
    std::memcpy(&name, buf, sizeof(name));
    if (!name.Buffer)
        return not_supported;

    // Waitable kinds the pool can satisfy without side effects on a
    // thread the coroutine does not own. Mutant (a mutex) is excluded
    // for that reason. File is excluded because a file object signals
    // on any I/O completion; console input and change notifications
    // are File objects and need their own handling.
    static constexpr std::wstring_view accepted[] = {
        L"Process", L"Thread", L"Event", L"Semaphore", L"Timer", L"Job"};
    std::wstring_view const type_name(
        name.Buffer, name.Length / sizeof(wchar_t));
    bool ok = false;
    for (auto a : accepted)
        ok = ok || type_name == a;
    if (!ok)
        return not_supported;

    return {};
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif
