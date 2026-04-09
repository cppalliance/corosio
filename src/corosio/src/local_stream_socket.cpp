//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/local_stream_service.hpp>

#include <sys/ioctl.h>

namespace boost::corosio {

local_stream_socket::~local_stream_socket()
{
    close();
}

local_stream_socket::local_stream_socket(capy::execution_context& ctx)
    : io_object(create_handle<detail::local_stream_service>(ctx))
{
}

void
local_stream_socket::open(local_stream proto)
{
    if (is_open())
        return;
    open_for_family(proto.family(), proto.type(), proto.protocol());
}

void
local_stream_socket::open_for_family(int family, int type, int protocol)
{
    auto& svc = static_cast<detail::local_stream_service&>(h_.service());
    std::error_code ec = svc.open_socket(
        static_cast<local_stream_socket::implementation&>(*h_.get()),
        family, type, protocol);
    if (ec)
        detail::throw_system_error(ec, "local_stream_socket::open");
}

void
local_stream_socket::close()
{
    if (!is_open())
        return;
    h_.service().close(h_);
}

void
local_stream_socket::cancel()
{
    if (!is_open())
        return;
    get().cancel();
}

void
local_stream_socket::shutdown(shutdown_type what)
{
    if (is_open())
    {
        // Best-effort: errors like ENOTCONN are expected and unhelpful
        [[maybe_unused]] auto ec = get().shutdown(what);
    }
}

void
local_stream_socket::shutdown(shutdown_type what, std::error_code& ec) noexcept
{
    ec = {};
    if (is_open())
        ec = get().shutdown(what);
}

void
local_stream_socket::assign(int fd)
{
    if (is_open())
        detail::throw_logic_error("assign: socket already open");
    auto& svc = static_cast<detail::local_stream_service&>(h_.service());
    std::error_code ec = svc.assign_socket(
        static_cast<local_stream_socket::implementation&>(*h_.get()), fd);
    if (ec)
        detail::throw_system_error(ec, "local_stream_socket::assign");
}

native_handle_type
local_stream_socket::native_handle() const noexcept
{
    if (!is_open())
        return -1;
    return get().native_handle();
}

native_handle_type
local_stream_socket::release()
{
    if (!is_open())
        detail::throw_logic_error("release: socket not open");
    return get().release_socket();
}

std::size_t
local_stream_socket::available() const
{
    if (!is_open())
        detail::throw_logic_error("available: socket not open");
    int value = 0;
    if (::ioctl(native_handle(), FIONREAD, &value) < 0)
        detail::throw_system_error(
            std::error_code(errno, std::system_category()),
            "local_stream_socket::available");
    return static_cast<std::size_t>(value);
}

local_endpoint
local_stream_socket::local_endpoint() const noexcept
{
    if (!is_open())
        return corosio::local_endpoint{};
    return get().local_endpoint();
}

local_endpoint
local_stream_socket::remote_endpoint() const noexcept
{
    if (!is_open())
        return corosio::local_endpoint{};
    return get().remote_endpoint();
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX
