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

#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/local_datagram_service.hpp>
#include <boost/corosio/native/detail/make_err.hpp>

#include <sys/ioctl.h>

namespace boost::corosio {

local_datagram_socket::~local_datagram_socket()
{
    close();
}

local_datagram_socket::local_datagram_socket(capy::execution_context& ctx)
    : io_object(create_handle<detail::local_datagram_service>(ctx))
{
}

std::error_code
local_datagram_socket::open(local_datagram proto) noexcept
{
    if (is_open())
        return {};
    return open_for_family(proto.family(), proto.type(), proto.protocol());
}

std::error_code
local_datagram_socket::open_for_family(int family, int type, int protocol) noexcept
{
    auto& svc = static_cast<detail::local_datagram_service&>(h_.service());
    std::error_code ec = svc.open_socket(
        static_cast<local_datagram_socket::implementation&>(*h_.get()),
        family, type, protocol);
    return ec;
}

void
local_datagram_socket::close() noexcept
{
    if (!is_open())
        return;
    h_.service().close(h_);
}

std::error_code
local_datagram_socket::bind(corosio::local_endpoint ep) noexcept
{
    if (!is_open())
        return make_error_code(std::errc::bad_file_descriptor);
    auto& svc = static_cast<detail::local_datagram_service&>(h_.service());
    return svc.bind_socket(
        static_cast<local_datagram_socket::implementation&>(*h_.get()),
        ep);
}

void
local_datagram_socket::cancel() noexcept
{
    if (!is_open())
        return;
    get().cancel();
}

std::error_code
local_datagram_socket::shutdown(shutdown_type what) noexcept
{
    if (!is_open())
        return make_error_code(std::errc::bad_file_descriptor);
    return get().shutdown(what);
}

std::error_code
local_datagram_socket::assign(native_handle_type fd) noexcept
{
    auto& svc = static_cast<detail::local_datagram_service&>(h_.service());
    std::error_code ec = svc.assign_socket(
        static_cast<local_datagram_socket::implementation&>(*h_.get()), fd);
    return ec;
}

native_handle_type
local_datagram_socket::native_handle() const noexcept
{
    if (!is_open())
        return -1;
    return get().native_handle();
}

native_handle_type
local_datagram_socket::release()
{
    if (!is_open())
        detail::throw_system_error(
            make_error_code(std::errc::bad_file_descriptor),
            "local_datagram_socket::release");
    return get().release_socket();
}

std::size_t
local_datagram_socket::available() const
{
    if (!is_open())
        detail::throw_system_error(
            make_error_code(std::errc::bad_file_descriptor),
            "local_datagram_socket::available");
    int value = 0;
    if (::ioctl(native_handle(), FIONREAD, &value) < 0)
        detail::throw_system_error(
            detail::make_err(errno),
            "local_datagram_socket::available");
    return static_cast<std::size_t>(value);
}

local_endpoint
local_datagram_socket::local_endpoint() const noexcept
{
    if (!is_open())
        return corosio::local_endpoint{};
    return get().local_endpoint();
}

local_endpoint
local_datagram_socket::remote_endpoint() const noexcept
{
    if (!is_open())
        return corosio::local_endpoint{};
    return get().remote_endpoint();
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX
