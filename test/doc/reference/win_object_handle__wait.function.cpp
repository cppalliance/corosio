//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/win_object_handle.hpp's
// documentation for win_object_handle::wait, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.
//
// win_object_handle's whole class body is wrapped in #if BOOST_COROSIO_HAS_IOCP
// in its own header, so the region that names the type is guarded the same
// way, following posix_stream_descriptor__wait.function.cpp. The corosio includes are
// safe unconditionally -- the header itself resolves to nothing off IOCP.

#include "../doc_warnings.hpp"

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/win_object_handle.hpp>

#include <boost/capy/task.hpp>

#include <system_error>

#if BOOST_COROSIO_HAS_IOCP
#include <windows.h>
#endif

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

#if BOOST_COROSIO_HAS_IOCP
// tag::wait[]
capy::task<std::error_code>
await_event(corosio::io_context& ioc, HANDLE event)
{
    // Adopt a duplicate: assign() takes ownership of the copy, so the
    // caller's own handle stays independent of this object's lifetime.
    // DUPLICATE_SAME_ACCESS keeps the caller's access, which must
    // include SYNCHRONIZE.
    HANDLE copy = nullptr;
    if (!::DuplicateHandle(
            ::GetCurrentProcess(), event, ::GetCurrentProcess(), &copy, 0,
            FALSE, DUPLICATE_SAME_ACCESS))
        co_return std::error_code(
            static_cast<int>(::GetLastError()), std::system_category());

    corosio::win_object_handle o(ioc);
    if (auto ec = o.assign(reinterpret_cast<corosio::native_handle_type>(copy)))
    {
        // A rejected handle stays the caller's to close.
        ::CloseHandle(copy);
        co_return ec;
    }

    // Resumes on ioc's executor once the event is signaled.
    auto [ec] = co_await o.wait();
    co_return ec;
}
// end::wait[]
#endif

} // namespace
