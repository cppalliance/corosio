//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/detail/local_stream_acceptor_service.hpp>

#include <cstring>

#if BOOST_COROSIO_POSIX
#include <unistd.h>
#else
// Windows: AF_UNIX socket files are reparse points with tag
// IO_REPARSE_TAG_AF_UNIX. DeleteFileA reliably removes them;
// std::filesystem::remove via libstdc++/MS STL was observed to
// silently leave them in place on at least some Windows hosts,
// so call the Win32 API directly.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace boost::corosio {

local_stream_acceptor::~local_stream_acceptor()
{
    close();
}

local_stream_acceptor::local_stream_acceptor(capy::execution_context& ctx)
    : io_object(create_handle<detail::local_stream_acceptor_service>(ctx))
    , ctx_(ctx)
{
}

void
local_stream_acceptor::open(local_stream proto)
{
    if (is_open())
        return;
    auto& svc =
        static_cast<detail::local_stream_acceptor_service&>(h_.service());
    auto ec = svc.open_acceptor_socket(
        static_cast<local_stream_acceptor::implementation&>(*h_.get()),
        proto.family(), proto.type(), proto.protocol());
    if (ec)
        detail::throw_system_error(ec, "local_stream_acceptor::open");
}

std::error_code
local_stream_acceptor::bind(corosio::local_endpoint ep, bind_option opt)
{
    if (!is_open())
        detail::throw_logic_error("bind: acceptor not open");

    if (opt == bind_option::unlink_existing &&
        !ep.empty() && !ep.is_abstract())
    {
        // Best-effort removal; missing file is fine.
        auto p = ep.path();
        char buf[local_endpoint::max_path_length + 1];
        std::memcpy(buf, p.data(), p.size());
        buf[p.size()] = '\0';
#if BOOST_COROSIO_POSIX
        ::unlink(buf);
#else
        ::DeleteFileA(buf);
#endif
    }

    auto& svc =
        static_cast<detail::local_stream_acceptor_service&>(h_.service());
    return svc.bind_acceptor(
        static_cast<local_stream_acceptor::implementation&>(*h_.get()),
        ep);
}

std::error_code
local_stream_acceptor::listen(int backlog)
{
    if (!is_open())
        detail::throw_logic_error("listen: acceptor not open");
    auto& svc =
        static_cast<detail::local_stream_acceptor_service&>(h_.service());
    return svc.listen_acceptor(
        static_cast<local_stream_acceptor::implementation&>(*h_.get()),
        backlog);
}

void
local_stream_acceptor::close()
{
    if (!is_open())
        return;
    h_.service().close(h_);
}

native_handle_type
local_stream_acceptor::release()
{
    if (!is_open())
        detail::throw_logic_error("release: acceptor not open");
    return get().release_socket();
}

void
local_stream_acceptor::cancel()
{
    if (!is_open())
        return;
    get().cancel();
}

local_endpoint
local_stream_acceptor::local_endpoint() const noexcept
{
    if (!is_open())
        return corosio::local_endpoint{};
    return get().local_endpoint();
}

} // namespace boost::corosio
