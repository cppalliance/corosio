//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/win_stream_handle.hpp's
// documentation for win_stream_handle, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.
//
// win_stream_handle's whole class body is wrapped in #if BOOST_COROSIO_HAS_IOCP
// in its own header, so the region that names the type is guarded the same
// way, following posix_stream_descriptor__wait.function.cpp. The corosio includes are
// safe unconditionally -- the header itself resolves to nothing off IOCP.

#include "../doc_warnings.hpp"

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/win_stream_handle.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <string_view>
#include <system_error>

#if BOOST_COROSIO_HAS_IOCP
#include <windows.h>
#endif

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

#if BOOST_COROSIO_HAS_IOCP
// tag::win_stream_handle[]
capy::task<std::error_code>
send_to_pipe_server(corosio::io_context& ioc, std::string_view request)
{
    // The client end of a named pipe. FILE_FLAG_OVERLAPPED is what
    // makes it adoptable; without it assign() rejects the handle.
    HANDLE h = ::CreateFileW(
        L"\\\\.\\pipe\\my_service", GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        co_return std::error_code(
            static_cast<int>(::GetLastError()), std::system_category());

    corosio::win_stream_handle pipe(ioc);
    if (auto ec = pipe.assign(reinterpret_cast<corosio::native_handle_type>(h)))
    {
        // A rejected handle stays the caller's to close.
        ::CloseHandle(h);
        co_return ec;
    }

    // Any capy::Stream algorithm works on the adopted handle.
    auto [ec, n] = co_await capy::write(
        pipe, capy::const_buffer(request.data(), request.size()));
    co_return ec;
}
// end::win_stream_handle[]
#endif

} // namespace
