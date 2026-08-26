//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "fault.hpp"
#include "fault_slot.hpp"

#if defined(_WIN32)

#include <boost/corosio/host_name.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <WinSock2.h>
#include <Windows.h>

#include <MSWSock.h>
#include <WS2tcpip.h>
#include <signal.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <tuple>

namespace boost::corosio::test::fault {

thread_local completion_slot tls_completion;

// The arm machinery lives in fault_arm.cpp; this is the one piece of
// it that has to know what platform it is on. Winsock and the kernel
// share the per-thread error slot, but the two setters are separate
// entry points and a hook cannot know which one its caller will read;
// publishing through both costs nothing on a path already failing.
void publish_error(int err) noexcept
{
    ::SetLastError(static_cast<DWORD>(err));
    ::WSASetLastError(err);
}

bool completion_should_fail(unsigned long& err) noexcept
{
    auto& c = tls_completion;
    if(!c.armed)
        return false;
    if(++c.seen != c.nth)
        return false;
    c.armed = false;
    c.fired = true;
    err = c.err;
    return true;
}

completion_fault_scope::completion_fault_scope(unsigned long err, unsigned nth)
{
    claim_completion_slot(tls_completion,
        "completion_fault_scope: a completion fault is already armed "
        "on this thread");
    tls_completion.err = err;
    tls_completion.nth = nth;
}

completion_fault_scope::~completion_fault_scope()
{
    release_completion_slot(tls_completion);
}

bool completion_fault_scope::fired() const noexcept
{
    return tls_completion.fired;
}

namespace {

// A generic function pointer. Every cast between an entry point's real
// type and this one goes through void(*)(), the one function type that
// -Wcast-function-type accepts in both directions.
using proc_t = void (*)();

// The entry points reached through an import table. Every hook is
// generated from this list: the id, the forwarding pointer slot, the
// hook itself and the table row all keep the list's order.
//
// `failval` is evaluated only after should_fail has published the
// armed error, which is what lets the entry points that report through
// their return value hand back ::GetLastError().
#define COROSIO_FAULT_WIN_SIMPLE(X)                                          \
    X(socket, SOCKET, INVALID_SOCKET, WSAAPI,                                \
        (int af, int type, int protocol), (af, type, protocol))              \
    X(WSASocketW, SOCKET, INVALID_SOCKET, WSAAPI,                            \
        (int af, int type, int protocol, LPWSAPROTOCOL_INFOW pi, GROUP g,    \
            DWORD flags),                                                    \
        (af, type, protocol, pi, g, flags))                                  \
    X(bind, int, SOCKET_ERROR, WSAAPI,                                       \
        (SOCKET s, sockaddr const* a, int len), (s, a, len))                 \
    X(listen, int, SOCKET_ERROR, WSAAPI, (SOCKET s, int backlog),            \
        (s, backlog))                                                        \
    X(accept, SOCKET, INVALID_SOCKET, WSAAPI,                                \
        (SOCKET s, sockaddr* a, int* len), (s, a, len))                      \
    X(connect, int, SOCKET_ERROR, WSAAPI,                                    \
        (SOCKET s, sockaddr const* a, int len), (s, a, len))                 \
    X(shutdown, int, SOCKET_ERROR, WSAAPI, (SOCKET s, int how), (s, how))    \
    X(closesocket, int, SOCKET_ERROR, WSAAPI, (SOCKET s), (s))               \
    X(ioctlsocket, int, SOCKET_ERROR, WSAAPI,                                \
        (SOCKET s, long cmd, u_long* argp), (s, cmd, argp))                  \
    X(getsockname, int, SOCKET_ERROR, WSAAPI,                                \
        (SOCKET s, sockaddr* a, int* len), (s, a, len))                      \
    X(getpeername, int, SOCKET_ERROR, WSAAPI,                                \
        (SOCKET s, sockaddr* a, int* len), (s, a, len))                      \
    X(getsockopt, int, SOCKET_ERROR, WSAAPI,                                 \
        (SOCKET s, int lvl, int name, char* val, int* len),                  \
        (s, lvl, name, val, len))                                            \
    X(setsockopt, int, SOCKET_ERROR, WSAAPI,                                 \
        (SOCKET s, int lvl, int name, char const* val, int len),             \
        (s, lvl, name, val, len))                                            \
    X(WSAConnect, int, SOCKET_ERROR, WSAAPI,                                 \
        (SOCKET s, sockaddr const* a, int len, LPWSABUF cd, LPWSABUF ud,     \
            LPQOS sq, LPQOS gq),                                             \
        (s, a, len, cd, ud, sq, gq))                                         \
    X(WSARecvFrom, int, SOCKET_ERROR, WSAAPI,                                \
        (SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD got, LPDWORD flags,       \
            sockaddr* from, LPINT fromlen, LPWSAOVERLAPPED ov,               \
            LPWSAOVERLAPPED_COMPLETION_ROUTINE cr),                          \
        (s, bufs, n, got, flags, from, fromlen, ov, cr))                     \
    X(WSASendTo, int, SOCKET_ERROR, WSAAPI,                                  \
        (SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD sent, DWORD flags,        \
            sockaddr const* to, int tolen, LPWSAOVERLAPPED ov,               \
            LPWSAOVERLAPPED_COMPLETION_ROUTINE cr),                          \
        (s, bufs, n, sent, flags, to, tolen, ov, cr))                        \
    X(WSAPoll, int, SOCKET_ERROR, WSAAPI,                                    \
        (LPWSAPOLLFD fds, ULONG n, INT timeout), (fds, n, timeout))          \
    X(WSAStartup, int, static_cast<int>(::GetLastError()), WSAAPI,           \
        (WORD ver, LPWSADATA data), (ver, data))                             \
    X(WSACleanup, int, SOCKET_ERROR, WSAAPI, (), ())                         \
    X(GetAddrInfoExW, INT, static_cast<INT>(::GetLastError()), WSAAPI,       \
        (PCWSTR name, PCWSTR service, DWORD ns, LPGUID nsid,                 \
            ADDRINFOEXW const* hints, PADDRINFOEXW* res, timeval* timeout,   \
            LPOVERLAPPED ov, LPLOOKUPSERVICE_COMPLETION_ROUTINE cr,          \
            LPHANDLE handle),                                                \
        (name, service, ns, nsid, hints, res, timeout, ov, cr, handle))      \
    X(GetAddrInfoExCancel, INT, static_cast<INT>(::GetLastError()), WSAAPI,  \
        (LPHANDLE handle), (handle))                                         \
    X(GetNameInfoW, INT, static_cast<INT>(::GetLastError()), WSAAPI,         \
        (sockaddr const* sa, int salen, wchar_t* node, DWORD nodelen,        \
            wchar_t* service, DWORD servicelen, INT flags),                  \
        (sa, salen, node, nodelen, service, servicelen, flags))              \
    X(CreateIoCompletionPort, HANDLE, nullptr, WINAPI,                       \
        (HANDLE file, HANDLE port, ULONG_PTR key, DWORD threads),            \
        (file, port, key, threads))                                          \
    X(PostQueuedCompletionStatus, BOOL, FALSE, WINAPI,                       \
        (HANDLE port, DWORD bytes, ULONG_PTR key, LPOVERLAPPED ov),          \
        (port, bytes, key, ov))                                              \
    X(CancelIoEx, BOOL, FALSE, WINAPI, (HANDLE h, LPOVERLAPPED ov), (h, ov)) \
    X(CloseHandle, BOOL, FALSE, WINAPI, (HANDLE h), (h))                     \
    X(CreateFileW, HANDLE, INVALID_HANDLE_VALUE, WINAPI,                     \
        (LPCWSTR name, DWORD access, DWORD share,                            \
            LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,               \
            HANDLE tmpl),                                                    \
        (name, access, share, sa, disp, flags, tmpl))                        \
    X(SetFilePointerEx, BOOL, FALSE, WINAPI,                                 \
        (HANDLE h, LARGE_INTEGER dist, PLARGE_INTEGER out, DWORD method),    \
        (h, dist, out, method))                                              \
    X(GetFileSizeEx, BOOL, FALSE, WINAPI,                                    \
        (HANDLE h, PLARGE_INTEGER size), (h, size))                          \
    X(SetEndOfFile, BOOL, FALSE, WINAPI, (HANDLE h), (h))                    \
    X(FlushFileBuffers, BOOL, FALSE, WINAPI, (HANDLE h), (h))                \
    X(DeleteFileA, BOOL, FALSE, WINAPI, (LPCSTR name), (name))               \
    X(CreateWaitableTimerW, HANDLE, nullptr, WINAPI,                         \
        (LPSECURITY_ATTRIBUTES sa, BOOL manual, LPCWSTR name),               \
        (sa, manual, name))                                                  \
    X(SetWaitableTimer, BOOL, FALSE, WINAPI,                                 \
        (HANDLE h, LARGE_INTEGER const* due, LONG period,                    \
            PTIMERAPCROUTINE apc, LPVOID arg, BOOL resume),                  \
        (h, due, period, apc, arg, resume))                                  \
    X(WaitForSingleObject, DWORD, WAIT_FAILED, WINAPI,                       \
        (HANDLE h, DWORD ms), (h, ms))                                       \
    X(GetComputerNameExW, BOOL, FALSE, WINAPI,                               \
        (COMPUTER_NAME_FORMAT kind, LPWSTR buf, LPDWORD size),               \
        (kind, buf, size))                                                   \
    X(GetModuleHandleA, HMODULE, nullptr, WINAPI, (LPCSTR name), (name))     \
    X(GetModuleHandleW, HMODULE, nullptr, WINAPI, (LPCWSTR name), (name))    \
    X(MultiByteToWideChar, int, 0, WINAPI,                                   \
        (UINT cp, DWORD flags, char const* in, int inlen, wchar_t* out,      \
            int outlen),                                                     \
        (cp, flags, in, inlen, out, outlen))                                 \
    X(WideCharToMultiByte, int, 0, WINAPI,                                   \
        (UINT cp, DWORD flags, wchar_t const* in, int inlen, char* out,      \
            int outlen, char const* dflt, LPBOOL used),                      \
        (cp, flags, in, inlen, out, outlen, dflt, used))

// Entry points whose hook does more than fail: it substitutes a
// pointer, rewrites a completion, or clamps a transfer.
#define COROSIO_FAULT_WIN_MANUAL(X)                                          \
    X(recv) X(send) X(WSARecv) X(WSASend) X(ReadFile) X(WriteFile)           \
    X(WSAIoctl) X(GetQueuedCompletionStatus) X(GetProcAddress)               \
    X(FreeAddrInfoExW) X(signal)

#define COROSIO_FAULT_WIN_ID(name, ret, failval, cc, params, args) h_##name,
#define COROSIO_FAULT_WIN_ID1(name) h_##name,

enum hook_id
{
    COROSIO_FAULT_WIN_SIMPLE(COROSIO_FAULT_WIN_ID)
    COROSIO_FAULT_WIN_MANUAL(COROSIO_FAULT_WIN_ID1)
    hook_count
};

// Filled in from the first module patched; every hook forwards through
// it. Kept out of the table so a hook body needs no forward reference
// to the table's type.
proc_t reals[hook_count] = {};

#define COROSIO_FAULT_WIN_CALL(name, ret, cc, params) \
    reinterpret_cast<ret(cc*) params>(reals[h_##name])

#define COROSIO_FAULT_WIN_HOOK(name, ret, failval, cc, params, args)        \
    ret cc hooked_##name params                                            \
    {                                                                       \
        if(should_fail(sys::name))                                          \
            return failval;                                                 \
        return COROSIO_FAULT_WIN_CALL(name, ret, cc, params) args;          \
    }

COROSIO_FAULT_WIN_SIMPLE(COROSIO_FAULT_WIN_HOOK)

// Copy the prefix of `in` holding at most `count` bytes into `out`.
// Corosio never passes more than a handful of buffers; 64 is a hard
// ceiling checked at runtime.
DWORD truncate_wsabuf(WSABUF const* in, DWORD n, std::size_t count,
    WSABUF* out) noexcept
{
    if(n > 64u)
        die("fault harness: WSABUF count exceeds 64");
    DWORD m = 0;
    for(; m < n && count > 0; ++m)
    {
        out[m] = in[m];
        if(out[m].len > count)
            out[m].len = static_cast<ULONG>(count);
        count -= out[m].len;
    }
    return m;
}

int WSAAPI hooked_recv(SOCKET s, char* buf, int len, int flags)
{
    if(should_fail(sys::recv))
        return SOCKET_ERROR;
    auto const real = COROSIO_FAULT_WIN_CALL(recv, int, WSAAPI,
        (SOCKET, char*, int, int));
    std::size_t c = 0;
    if(should_shorten(sys::recv, c))
    {
        if(c == 0)
            return 0;
        return real(s, buf, static_cast<int>(c) < len ? static_cast<int>(c)
            : len, flags);
    }
    return real(s, buf, len, flags);
}

int WSAAPI hooked_send(SOCKET s, char const* buf, int len, int flags)
{
    if(should_fail(sys::send))
        return SOCKET_ERROR;
    auto const real = COROSIO_FAULT_WIN_CALL(send, int, WSAAPI,
        (SOCKET, char const*, int, int));
    std::size_t c = 0;
    if(should_shorten(sys::send, c))
    {
        if(c == 0)
            return 0;
        return real(s, buf, static_cast<int>(c) < len ? static_cast<int>(c)
            : len, flags);
    }
    return real(s, buf, len, flags);
}

int WSAAPI hooked_WSARecv(SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD got,
    LPDWORD flags, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    if(should_fail(sys::WSARecv))
        return SOCKET_ERROR;
    auto const real = COROSIO_FAULT_WIN_CALL(WSARecv, int, WSAAPI,
        (SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED,
            LPWSAOVERLAPPED_COMPLETION_ROUTINE));
    std::size_t c = 0;
    if(should_shorten(sys::WSARecv, c))
    {
        // A zero-length receive completes with zero bytes even with
        // data waiting, which is what the stream layer reads as EOF.
        WSABUF t[64];
        DWORD m = truncate_wsabuf(bufs, n, c, t);
        if(m == 0)
        {
            t[0].len = 0;
            t[0].buf = (bufs && n > 0) ? bufs[0].buf : nullptr;
            m = 1;
        }
        return real(s, t, m, got, flags, ov, cr);
    }
    return real(s, bufs, n, got, flags, ov, cr);
}

int WSAAPI hooked_WSASend(SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD sent,
    DWORD flags, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    if(should_fail(sys::WSASend))
        return SOCKET_ERROR;
    auto const real = COROSIO_FAULT_WIN_CALL(WSASend, int, WSAAPI,
        (SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED,
            LPWSAOVERLAPPED_COMPLETION_ROUTINE));
    std::size_t c = 0;
    if(should_shorten(sys::WSASend, c))
    {
        WSABUF t[64];
        DWORD m = truncate_wsabuf(bufs, n, c, t);
        if(m == 0)
        {
            t[0].len = 0;
            t[0].buf = (bufs && n > 0) ? bufs[0].buf : nullptr;
            m = 1;
        }
        return real(s, t, m, sent, flags, ov, cr);
    }
    return real(s, bufs, n, sent, flags, ov, cr);
}

BOOL WINAPI hooked_ReadFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD got,
    LPOVERLAPPED ov)
{
    if(should_fail(sys::ReadFile))
        return FALSE;
    auto const real = COROSIO_FAULT_WIN_CALL(ReadFile, BOOL, WINAPI,
        (HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED));
    std::size_t c = 0;
    if(should_shorten(sys::ReadFile, c))
        return real(h, buf, static_cast<DWORD>(c) < len
            ? static_cast<DWORD>(c) : len, got, ov);
    return real(h, buf, len, got, ov);
}

BOOL WINAPI hooked_WriteFile(HANDLE h, LPCVOID buf, DWORD len, LPDWORD put,
    LPOVERLAPPED ov)
{
    if(should_fail(sys::WriteFile))
        return FALSE;
    auto const real = COROSIO_FAULT_WIN_CALL(WriteFile, BOOL, WINAPI,
        (HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED));
    std::size_t c = 0;
    if(should_shorten(sys::WriteFile, c))
        return real(h, buf, static_cast<DWORD>(c) < len
            ? static_cast<DWORD>(c) : len, put, ov);
    return real(h, buf, len, put, ov);
}

void WSAAPI hooked_FreeAddrInfoExW(PADDRINFOEXW ai)
{
    // Nothing to report through: the arm only records that the release
    // path ran, and swallowing the call would leak.
    std::ignore = should_fail(sys::FreeAddrInfoExW);
    COROSIO_FAULT_WIN_CALL(FreeAddrInfoExW, void, WSAAPI, (PADDRINFOEXW))(ai);
}

using sig_handler_t = void(__cdecl*)(int);

sig_handler_t __cdecl hooked_signal(int sig, sig_handler_t handler)
{
    if(should_fail(sys::signal))
        return SIG_ERR;
    return COROSIO_FAULT_WIN_CALL(signal, sig_handler_t, __cdecl,
        (int, sig_handler_t))(sig, handler);
}

// The pointers the OS hands out rather than exports. Written once each
// by the WSAIoctl / GetProcAddress hooks; a second io_context stores
// the same value, so the race is benign.
LPFN_ACCEPTEX real_accept_ex = nullptr;
LPFN_CONNECTEX real_connect_ex = nullptr;
proc_t real_nt_set_information_file = nullptr;
proc_t real_nt_flush_buffers_file_ex = nullptr;

BOOL PASCAL hooked_AcceptEx(SOCKET listener, SOCKET accepted, PVOID buf,
    DWORD recv_len, DWORD local_len, DWORD remote_len, LPDWORD got,
    LPOVERLAPPED ov)
{
    if(should_fail(sys::AcceptEx))
        return FALSE;
    return real_accept_ex(listener, accepted, buf, recv_len, local_len,
        remote_len, got, ov);
}

BOOL PASCAL hooked_ConnectEx(SOCKET s, sockaddr const* name, int namelen,
    PVOID buf, DWORD buf_len, LPDWORD sent, LPOVERLAPPED ov)
{
    if(should_fail(sys::ConnectEx))
        return FALSE;
    return real_connect_ex(s, name, namelen, buf, buf_len, sent, ov);
}

// The library only tests these against zero, so one non-zero NTSTATUS
// is as good as any: the arm's `err` reaches the caller through the
// last-error slot instead.
LONG const status_unsuccessful = static_cast<LONG>(0xC0000001);

LONG NTAPI hooked_NtSetInformationFile(HANDLE h, ULONG_PTR* iosb, void* info,
    ULONG len, ULONG cls)
{
    if(should_fail(sys::NtSetInformationFile))
        return status_unsuccessful;
    return reinterpret_cast<LONG(NTAPI*)(HANDLE, ULONG_PTR*, void*, ULONG,
        ULONG)>(real_nt_set_information_file)(h, iosb, info, len, cls);
}

LONG NTAPI hooked_NtFlushBuffersFileEx(HANDLE h, ULONG flags, void* params,
    ULONG len, void* iosb)
{
    if(should_fail(sys::NtFlushBuffersFileEx))
        return status_unsuccessful;
    return reinterpret_cast<LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG,
        void*)>(real_nt_flush_buffers_file_ex)(h, flags, params, len, iosb);
}

bool same_guid(void const* lhs, GUID const& rhs) noexcept
{
    return std::memcmp(lhs, &rhs, sizeof(GUID)) == 0;
}

int WSAAPI hooked_WSAIoctl(SOCKET s, DWORD code, LPVOID in, DWORD inlen,
    LPVOID out, DWORD outlen, LPDWORD ret, LPWSAOVERLAPPED ov,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    if(should_fail(sys::WSAIoctl))
        return SOCKET_ERROR;
    int const r = COROSIO_FAULT_WIN_CALL(WSAIoctl, int, WSAAPI,
        (SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD,
            LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE))(
        s, code, in, inlen, out, outlen, ret, ov, cr);
    if(r != 0 || code != SIO_GET_EXTENSION_FUNCTION_POINTER ||
        !in || inlen < sizeof(GUID) || !out || outlen < sizeof(void*))
        return r;
    GUID const accept_ex = WSAID_ACCEPTEX;
    GUID const connect_ex = WSAID_CONNECTEX;
    if(same_guid(in, accept_ex))
    {
        std::memcpy(&real_accept_ex, out, sizeof(real_accept_ex));
        auto const p = &hooked_AcceptEx;
        std::memcpy(out, &p, sizeof(p));
    }
    else if(same_guid(in, connect_ex))
    {
        std::memcpy(&real_connect_ex, out, sizeof(real_connect_ex));
        auto const p = &hooked_ConnectEx;
        std::memcpy(out, &p, sizeof(p));
    }
    return r;
}

FARPROC WINAPI hooked_GetProcAddress(HMODULE mod, LPCSTR name)
{
    if(should_fail(sys::GetProcAddress))
        return nullptr;
    FARPROC const p = COROSIO_FAULT_WIN_CALL(GetProcAddress, FARPROC, WINAPI,
        (HMODULE, LPCSTR))(mod, name);
    // An ordinal import carries no string to compare.
    if(!p || IS_INTRESOURCE(name))
        return p;
    if(std::strcmp(name, "NtSetInformationFile") == 0)
    {
        real_nt_set_information_file = reinterpret_cast<proc_t>(p);
        return reinterpret_cast<FARPROC>(
            reinterpret_cast<proc_t>(&hooked_NtSetInformationFile));
    }
    if(std::strcmp(name, "NtFlushBuffersFileEx") == 0)
    {
        real_nt_flush_buffers_file_ex = reinterpret_cast<proc_t>(p);
        return reinterpret_cast<FARPROC>(
            reinterpret_cast<proc_t>(&hooked_NtFlushBuffersFileEx));
    }
    return p;
}

// win_scheduler reads the dequeue's outcome as
// `SetLastError(0); r = GetQueuedCompletionStatus(...);
//  err = r ? 0 : GetLastError()`, and never looks at OVERLAPPED's own
// status, so failing the call after it succeeded is what puts an error
// on a completed operation. Leaving the overlapped pointer null
// instead reaches the scheduler's own throw path.
BOOL WINAPI hooked_GetQueuedCompletionStatus(HANDLE port, LPDWORD bytes,
    PULONG_PTR key, LPOVERLAPPED* ov, DWORD ms)
{
    if(should_fail(sys::GetQueuedCompletionStatus))
    {
        if(ov)
            *ov = nullptr;
        return FALSE;
    }
    BOOL const r = COROSIO_FAULT_WIN_CALL(GetQueuedCompletionStatus, BOOL,
        WINAPI, (HANDLE, LPDWORD, PULONG_PTR, LPOVERLAPPED*, DWORD))(
        port, bytes, key, ov, ms);
    unsigned long err = 0;
    if(r && ov && *ov && completion_should_fail(err))
    {
        ::SetLastError(static_cast<DWORD>(err));
        return FALSE;
    }
    return r;
}

struct hook_entry
{
    char const* name;
    sys which;
    proc_t hook;
    // Import thunks now pointing at the hook, summed over the modules
    // that carry corosio code.
    unsigned bound;
};

#define COROSIO_FAULT_WIN_ROW(name, ret, failval, cc, params, args)          \
    { #name, sys::name, reinterpret_cast<proc_t>(&hooked_##name), 0 },
#define COROSIO_FAULT_WIN_ROW1(name)                                         \
    { #name, sys::name, reinterpret_cast<proc_t>(&hooked_##name), 0 },

hook_entry hooks[] = {
    COROSIO_FAULT_WIN_SIMPLE(COROSIO_FAULT_WIN_ROW)
    COROSIO_FAULT_WIN_MANUAL(COROSIO_FAULT_WIN_ROW1)
};

static_assert(sizeof(hooks) / sizeof(hooks[0]) == hook_count,
    "the hook table and the hook ids disagree");

// Match by function name alone rather than by (dll, name). A given
// entry point moves between kernel32, KERNELBASE and the
// api-ms-win-core-* forwarders from one toolchain to the next, and the
// CRT's `signal` between ucrtbase, msvcrt and the api-ms-win-crt-*
// forwarders; none of the names here is exported by two unrelated DLLs.
hook_entry* find_hook(char const* name) noexcept
{
    for(auto& h : hooks)
    {
        if(std::strcmp(h.name, name) == 0)
            return &h;
    }
    return nullptr;
}

char const* module_name(HMODULE mod) noexcept
{
    static char buf[MAX_PATH + 1];
    if(!::GetModuleFileNameA(mod, buf, MAX_PATH))
        std::snprintf(buf, sizeof(buf), "<module %p>",
            reinterpret_cast<void*>(mod));
    return buf;
}

// True for the Winsock DLL, whose import library is the only one here
// that binds by ordinal.
bool name_is_ws2_32(char const* dll) noexcept
{
    static char const needle[] = "ws2_32";
    for(std::size_t i = 0; i < sizeof(needle) - 1; ++i)
    {
        char c = dll[i];
        if(c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if(c != needle[i])
            return false;
    }
    return true;
}

// ws2_32.lib binds the Winsock 1.1 entry points by ordinal rather than
// by name, so their thunks carry no string for the name walk to match.
// The ordinals have been fixed since NT 4 — that is what an ordinal
// import is for — and the ones this table omits are entry points the
// harness does not hook.
char const* winsock_ordinal_name(char const* dll, unsigned ordinal) noexcept
{
    if(!name_is_ws2_32(dll))
        return nullptr;
    switch(ordinal)
    {
    case 1: return "accept";
    case 2: return "bind";
    case 3: return "closesocket";
    case 4: return "connect";
    case 5: return "getpeername";
    case 6: return "getsockname";
    case 7: return "getsockopt";
    case 10: return "ioctlsocket";
    case 13: return "listen";
    case 16: return "recv";
    case 19: return "send";
    case 21: return "setsockopt";
    case 22: return "shutdown";
    case 23: return "socket";
    case 115: return "WSAStartup";
    case 116: return "WSACleanup";
    default: return nullptr;
    }
}

// Walk one module's import descriptors, handing every import the
// harness can identify to `f` along with the thunk that holds its
// resolved address.
template<class F>
void for_each_import(HMODULE mod, F&& f) noexcept
{
    auto* const base = reinterpret_cast<std::uint8_t*>(mod);
    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    if(dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    auto const* nt =
        reinterpret_cast<IMAGE_NT_HEADERS const*>(base + dos->e_lfanew);
    if(nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    auto const& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(dir.VirtualAddress == 0)
        return;
    for(auto const* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR const*>(
            base + dir.VirtualAddress);
        imp->Name != 0; ++imp)
    {
        // Without the lookup table the thunks hold addresses, not
        // names, and nothing here can be identified.
        if(imp->OriginalFirstThunk == 0)
            continue;
        char const* const dll =
            reinterpret_cast<char const*>(base + imp->Name);
        auto const* names = reinterpret_cast<IMAGE_THUNK_DATA const*>(
            base + imp->OriginalFirstThunk);
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + imp->FirstThunk);
        for(; names->u1.AddressOfData != 0; ++names, ++thunk)
        {
            char const* name = nullptr;
            if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
            {
                name = winsock_ordinal_name(dll, static_cast<unsigned>(
                    IMAGE_ORDINAL(names->u1.Ordinal)));
            }
            else
            {
                auto const* by_name =
                    reinterpret_cast<IMAGE_IMPORT_BY_NAME const*>(
                        base + names->u1.AddressOfData);
                name = reinterpret_cast<char const*>(by_name->Name);
            }
            if(name)
                f(name, *thunk);
        }
    }
}

void patch_module(HMODULE mod) noexcept
{
    for_each_import(mod, [](char const* name, IMAGE_THUNK_DATA& thunk)
    {
        hook_entry* h = find_hook(name);
        if(!h)
            return;
        auto const idx = static_cast<std::size_t>(h - hooks);
        if(!reals[idx])
            reals[idx] = reinterpret_cast<proc_t>(thunk.u1.Function);
        DWORD old = 0;
        if(!::VirtualProtect(&thunk.u1.Function, sizeof(void*),
            PAGE_READWRITE, &old))
        {
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                "fault harness: the import thunk for %s refused to become "
                "writable", name);
            die(msg);
        }
        thunk.u1.Function = reinterpret_cast<ULONG_PTR>(h->hook);
        std::ignore = ::VirtualProtect(&thunk.u1.Function, sizeof(void*),
            old, &old);
    });
}

// Re-read the memory as it stands rather than trusting what the patch
// pass believed it wrote: a thunk that silently refused the store, or
// a second thunk for the same name that the walk skipped, would leave
// the arm dead with nothing to say so.
void verify_module(HMODULE mod, bool& ok) noexcept
{
    for_each_import(mod, [&](char const* name, IMAGE_THUNK_DATA& thunk)
    {
        hook_entry* h = find_hook(name);
        if(!h)
            return;
        if(thunk.u1.Function == reinterpret_cast<ULONG_PTR>(h->hook))
        {
            ++h->bound;
            return;
        }
        std::fprintf(stderr, "fault harness: %s in %s is bound to %p, hook "
            "is %p\n", name, module_name(mod),
            reinterpret_cast<void*>(thunk.u1.Function),
            reinterpret_cast<void*>(h->hook));
        ok = false;
    });
}

// Case-insensitive substring match, hand-rolled because the CRT spells
// its wide comparison differently on every toolchain.
bool name_holds_corosio(wchar_t const* name) noexcept
{
    static wchar_t const needle[] = L"corosio";
    for(; *name; ++name)
    {
        std::size_t i = 0;
        for(; needle[i]; ++i)
        {
            wchar_t c = name[i];
            if(c >= L'A' && c <= L'Z')
                c = static_cast<wchar_t>(c - L'A' + L'a');
            if(c != needle[i])
                break;
        }
        if(!needle[i])
            return true;
    }
    return false;
}

// Every module that can hold corosio code. The IOCP backend is header
// inline, so a shared build calls the OS from two import tables: the
// DLL's, reached by plain io_context/tcp_socket through the backend's
// virtual dispatch, and the executable's, reached by the native_*
// wrappers instantiated in the test.
std::size_t collect_modules(HMODULE* out, std::size_t cap) noexcept
{
    std::size_t n = 0;
    auto add = [&](HMODULE m)
    {
        if(!m)
            return;
        for(std::size_t i = 0; i < n; ++i)
        {
            if(out[i] == m)
                return;
        }
        if(n < cap)
            out[n++] = m;
    };
    add(::GetModuleHandleW(nullptr));

    // host_name is an ordinary exported corosio function, so the module
    // owning its address is the library wherever it ended up. This
    // needs no name and cannot be fooled by an unrelated module.
    HMODULE lib = nullptr;
    auto const* addr =
        reinterpret_cast<void const*>(&boost::corosio::host_name);
    if(::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCWSTR>(addr), &lib))
        add(lib);

    // A corosio satellite (a TLS backend, say) would carry inline
    // backend code of its own; a snapshot is the only way to see it.
    HANDLE const snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if(snap != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W me{};
        me.dwSize = static_cast<DWORD>(sizeof(me));
        if(::Module32FirstW(snap, &me))
        {
            do
            {
                if(name_holds_corosio(me.szModule))
                    add(me.hModule);
            }
            while(::Module32NextW(snap, &me));
        }
        ::CloseHandle(snap);
    }
    return n;
}

bool shared_build() noexcept
{
    HMODULE lib = nullptr;
    auto const* addr =
        reinterpret_cast<void const*>(&boost::corosio::host_name);
    if(!::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCWSTR>(addr), &lib))
        return false;
    return lib != ::GetModuleHandleW(nullptr);
}

int const installed = []
{
    HMODULE mods[8];
    std::size_t const n = collect_modules(mods, sizeof(mods) / sizeof(mods[0]));
    for(std::size_t i = 0; i < n; ++i)
        patch_module(mods[i]);

    bool ok = true;
    for(std::size_t i = 0; i < n; ++i)
        verify_module(mods[i], ok);

    for(auto const& h : hooks)
    {
        if(h.bound != 0)
            continue;
        // Reported rather than fatal: what a given toolchain imports
        // varies, and a name nothing in this program references has no
        // thunk to patch. The self-tests skip a hook that is not live,
        // so an unreported drift cannot masquerade as coverage.
        std::fprintf(stderr,
            "fault harness: %s is imported by no corosio module\n", h.name);
    }
    if(!ok)
        die("fault harness: the import tables were not patched");
    return 0;
}();

} // namespace

bool corosio_is_shared() noexcept
{
    return shared_build();
}

bool hook_is_live(sys which) noexcept
{
    switch(which)
    {
    // Substituted through the pointer WSAIoctl hands out.
    case sys::AcceptEx:
    case sys::ConnectEx:
        return hooks[h_WSAIoctl].bound != 0;
    // Substituted through the pointer GetProcAddress hands out.
    case sys::NtSetInformationFile:
    case sys::NtFlushBuffersFileEx:
        return hooks[h_GetProcAddress].bound != 0;
    default:
        break;
    }
    for(auto const& h : hooks)
    {
        if(h.which == which)
            return h.bound != 0;
    }
    return false;
}

} // boost::corosio::test::fault

#endif
