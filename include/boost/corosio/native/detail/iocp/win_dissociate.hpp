//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_DISSOCIATE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_DISSOCIATE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/native/detail/iocp/win_windows.hpp>

namespace boost::corosio::detail {

/** Detach a socket from its I/O completion port.

    The documented API keeps a handle bound to its completion port for
    the handle's lifetime; `NtSetInformationFile` with the
    `FileReplaceCompletionInformation` class is the only way to sever
    the association. Without this, a released socket can never be
    adopted into an io_context again: re-association fails with
    `ERROR_INVALID_PARAMETER`.

    @param s The socket to detach.

    @return `true` if the association was removed.
*/
inline bool
dissociate_from_iocp(SOCKET s) noexcept
{
    using nt_set_information_file_fn =
        LONG(NTAPI*)(HANDLE, ULONG_PTR*, void*, ULONG, ULONG);

    static nt_set_information_file_fn const fn =
        []() noexcept -> nt_set_information_file_fn {
        if (HMODULE h = ::GetModuleHandleW(L"ntdll.dll"))
        {
            // The two-step cast through void(*)() is the sanctioned
            // FARPROC conversion; a direct cast trips
            // -Wcast-function-type.
            return reinterpret_cast<nt_set_information_file_fn>(
                reinterpret_cast<void (*)()>(
                    ::GetProcAddress(h, "NtSetInformationFile")));
        }
        return nullptr;
    }();
    if (!fn)
        return false;

    // FILE_COMPLETION_INFORMATION{ nullptr, nullptr } under info
    // class FileReplaceCompletionInformation (61).
    ULONG_PTR iosb[2] = {0, 0};
    void* info[2]     = {nullptr, nullptr};
    return fn(reinterpret_cast<HANDLE>(s), iosb, &info, sizeof(info), 61) ==
        0;
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_DISSOCIATE_HPP
