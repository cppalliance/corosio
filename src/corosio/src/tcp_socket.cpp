//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_tcp_acceptor_service.hpp>
#else
#include <boost/corosio/detail/tcp_service.hpp>
#endif

namespace boost::corosio {

tcp_socket::~tcp_socket()
{
    close();
}

tcp_socket::tcp_socket(capy::execution_context& ctx)
#if BOOST_COROSIO_HAS_IOCP
    : io_object(create_handle<detail::win_tcp_service>(ctx))
#else
    : io_object(create_handle<detail::tcp_service>(ctx))
#endif
{
}

std::error_code
tcp_socket::open(tcp proto) noexcept
{
    if (is_open())
        return {};
    return open_for_family(proto.family(), proto.type(), proto.protocol());
}

std::error_code
tcp_socket::open_for_family(int family, int type, int protocol) noexcept
{
#if BOOST_COROSIO_HAS_IOCP
    auto& svc          = static_cast<detail::win_tcp_service&>(h_.service());
    auto& wrapper      = static_cast<tcp_socket::implementation&>(*h_.get());
    std::error_code ec = svc.open_socket(
        *static_cast<detail::win_tcp_socket&>(wrapper).get_internal(), family, type,
        protocol);
#else
    auto& svc          = static_cast<detail::tcp_service&>(h_.service());
    std::error_code ec = svc.open_socket(
        static_cast<tcp_socket::implementation&>(*h_.get()), family, type,
        protocol);
#endif
    return ec;
}

std::error_code
tcp_socket::assign(native_handle_type fd) noexcept
{
#if BOOST_COROSIO_HAS_IOCP
    auto& svc          = static_cast<detail::win_tcp_service&>(h_.service());
    auto& wrapper      = static_cast<tcp_socket::implementation&>(*h_.get());
    std::error_code ec = svc.assign_socket(
        *static_cast<detail::win_tcp_socket&>(wrapper).get_internal(), fd);
#else
    auto& svc          = static_cast<detail::tcp_service&>(h_.service());
    std::error_code ec = svc.assign_socket(
        static_cast<tcp_socket::implementation&>(*h_.get()), fd);
#endif
    return ec;
}

native_handle_type
tcp_socket::release()
{
    if (!is_open())
        detail::throw_system_error(
            make_error_code(std::errc::bad_file_descriptor),
            "tcp_socket::release");
    return get().release_socket();
}

std::error_code
tcp_socket::bind(endpoint ep) noexcept
{
    if (!is_open())
        return make_error_code(std::errc::bad_file_descriptor);
#if BOOST_COROSIO_HAS_IOCP
    auto& svc     = static_cast<detail::win_tcp_service&>(h_.service());
    auto& wrapper = static_cast<tcp_socket::implementation&>(*h_.get());
    return svc.bind_socket(
        *static_cast<detail::win_tcp_socket&>(wrapper).get_internal(), ep);
#else
    auto& svc = static_cast<detail::tcp_service&>(h_.service());
    return svc.bind_socket(
        static_cast<tcp_socket::implementation&>(*h_.get()), ep);
#endif
}

void
tcp_socket::close() noexcept
{
    if (!is_open())
        return;
    h_.service().close(h_);
}

void
tcp_socket::cancel() noexcept
{
    if (!is_open())
        return;
    get().cancel();
}

std::error_code
tcp_socket::shutdown(shutdown_type what) noexcept
{
    if (!is_open())
        return make_error_code(std::errc::bad_file_descriptor);
    return get().shutdown(what);
}

native_handle_type
tcp_socket::native_handle() const noexcept
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
tcp_socket::local_endpoint() const noexcept
{
    if (!is_open())
        return endpoint{};
    return get().local_endpoint();
}

endpoint
tcp_socket::remote_endpoint() const noexcept
{
    if (!is_open())
        return endpoint{};
    return get().remote_endpoint();
}

} // namespace boost::corosio
