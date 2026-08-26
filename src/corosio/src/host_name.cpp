//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/host_name.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/native/detail/make_err.hpp>

#include <string>
#include <system_error>

#if BOOST_COROSIO_POSIX
#include <cerrno>
#include <cstring>
#include <unistd.h>
#elif BOOST_COROSIO_HAS_IOCP
#include <windows.h>
#endif

namespace boost::corosio {

#if BOOST_COROSIO_POSIX

capy::io_result<std::string>
host_name()
{
    // 256 exceeds POSIX's _POSIX_HOST_NAME_MAX floor of 255 and
    // every mainstream OS's actual cap (Linux 64, macOS/BSD 255).
    char buf[256];
    if (::gethostname(buf, sizeof(buf)) != 0)
        return {detail::make_err(errno), {}};

    // POSIX does not guarantee NUL termination on truncation.
    if (std::memchr(buf, '\0', sizeof(buf)) == nullptr)
        return {make_error_code(std::errc::value_too_large), {}};

    return {std::error_code{}, std::string(buf)};
}

#elif BOOST_COROSIO_HAS_IOCP

capy::io_result<std::string>
host_name()
{
    // Size query: returns ERROR_MORE_DATA and writes the required
    // wide-char count (including the trailing NUL) into `size`.
    DWORD size = 0;
    BOOL ok = ::GetComputerNameExW(
        ComputerNameDnsHostname, nullptr, &size);
    DWORD err = ::GetLastError();
    if (ok)
    {
        // Can't-happen guard: a zero-length size query succeeding
        // would leave `size` meaningless.
        return {make_error_code(std::errc::protocol_error), {}};
    }
    if (err != ERROR_MORE_DATA)
        return {
            detail::make_err(static_cast<unsigned long>(err)),
            {}};

    // On success, GetComputerNameExW rewrites `size` to the count
    // without the NUL, so resize(size) below trims to the hostname.
    std::wstring wide(size, L'\0');
    if (!::GetComputerNameExW(
            ComputerNameDnsHostname, wide.data(), &size))
        return {
            detail::make_err(
                static_cast<unsigned long>(::GetLastError())),
            {}};
    wide.resize(size);

    int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {
            detail::make_err(
                static_cast<unsigned long>(::GetLastError())),
            {}};

    std::string out(static_cast<std::size_t>(needed), '\0');
    int written = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        out.data(), needed, nullptr, nullptr);
    if (written != needed)
        return {
            detail::make_err(
                static_cast<unsigned long>(::GetLastError())),
            {}};
    return {std::error_code{}, std::move(out)};
}

#endif

} // namespace boost::corosio
