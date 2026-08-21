//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/platform.hpp>

#include <boost/corosio/detail/udp_service.hpp>

namespace boost::corosio {

udp_socket::~udp_socket()
{
    close();
}

udp_socket::udp_socket(capy::execution_context& ctx)
    : io_object(create_handle<detail::udp_service>(ctx))
{
}

std::error_code
udp_socket::open(udp proto) noexcept
{
    if (is_open())
        return {};
    return open_for_family(proto.family(), proto.type(), proto.protocol());
}

std::error_code
udp_socket::open_for_family(int family, int type, int protocol) noexcept
{
    auto& svc          = static_cast<detail::udp_service&>(h_.service());
    std::error_code ec = svc.open_datagram_socket(
        static_cast<udp_socket::implementation&>(*h_.get()), family, type,
        protocol);
    return ec;
}

std::error_code
udp_socket::assign(native_handle_type fd) noexcept
{
    auto& svc          = static_cast<detail::udp_service&>(h_.service());
    std::error_code ec = svc.assign_socket(
        static_cast<udp_socket::implementation&>(*h_.get()), fd);
    return ec;
}

native_handle_type
udp_socket::release()
{
    if (!is_open())
        detail::throw_system_error(
            make_error_code(std::errc::bad_file_descriptor),
            "udp_socket::release");
    return get().release_socket();
}

void
udp_socket::close() noexcept
{
    if (!is_open())
        return;
    h_.service().close(h_);
}

std::error_code
udp_socket::bind(endpoint ep) noexcept
{
    if (!is_open())
        return make_error_code(std::errc::bad_file_descriptor);
    auto& svc = static_cast<detail::udp_service&>(h_.service());
    return svc.bind_datagram(
        static_cast<udp_socket::implementation&>(*h_.get()), ep);
}

std::error_code
udp_socket::shutdown(shutdown_type what) noexcept
{
    if (!is_open())
        return make_error_code(std::errc::bad_file_descriptor);
    return get().shutdown(what);
}

void
udp_socket::cancel()
{
    if (!is_open())
        return;
    get().cancel();
}

native_handle_type
udp_socket::native_handle() const noexcept
{
    if (!is_open())
    {
#if BOOST_COROSIO_HAS_IOCP
        return static_cast<native_handle_type>(~0ull); // INVALID_SOCKET
#else
        return -1;
#endif
    }
    return get().native_handle();
}

endpoint
udp_socket::local_endpoint() const noexcept
{
    if (!is_open())
        return endpoint{};
    return get().local_endpoint();
}

endpoint
udp_socket::remote_endpoint() const noexcept
{
    if (!is_open())
        return endpoint{};
    return get().remote_endpoint();
}

} // namespace boost::corosio
