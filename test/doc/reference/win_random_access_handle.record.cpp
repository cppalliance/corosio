//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into
// include/boost/corosio/win_random_access_handle.hpp's documentation for
// win_random_access_handle, by doc/addons/extensions/reference-snippets.lua.
// The tagged region is what the reference renders; scaffolding stays outside
// the tags.
//
// win_random_access_handle's whole class body is wrapped in
// #if BOOST_COROSIO_HAS_IOCP in its own header, so the region that names the
// type is guarded the same way, following win_stream_handle.record.cpp. The
// corosio includes are safe unconditionally -- the header itself resolves to
// nothing off IOCP.

#include "../doc_warnings.hpp"

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/win_random_access_handle.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>

#include <cstddef>
#include <memory>
#include <system_error>

#if BOOST_COROSIO_HAS_IOCP
#include <windows.h>
#endif

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

#if BOOST_COROSIO_HAS_IOCP
// tag::win_random_access_handle[]
capy::task<std::error_code>
read_volume_start(corosio::io_context& ioc)
{
    // The C: volume. FILE_FLAG_OVERLAPPED is what makes it adoptable;
    // without it assign() rejects the handle. Opening a volume
    // requires administrator rights.
    HANDLE h = ::CreateFileW(
        L"\\\\.\\C:", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        co_return std::error_code(
            static_cast<int>(::GetLastError()), std::system_category());

    corosio::win_random_access_handle volume(ioc);
    if (auto ec = volume.assign(
            reinterpret_cast<corosio::native_handle_type>(h)))
    {
        // A rejected handle stays the caller's to close.
        ::CloseHandle(h);
        co_return ec;
    }

    // Volume I/O must be sector-aligned in offset, length, and buffer.
    // 4096 is a multiple of every common sector size.
    struct alignas(4096) sector
    {
        std::byte bytes[4096];
    };
    auto buf = std::make_unique<sector>();

    auto [ec, n] = co_await volume.read_some_at(
        0, capy::mutable_buffer(buf->bytes, sizeof(buf->bytes)));
    co_return ec;
}
// end::win_random_access_handle[]
#endif

} // namespace
