//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/error.hpp>
#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/platform.hpp>

#include "src/detail/use_backend_service.hpp"

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_tcp_acceptor_service.hpp>
#else
#include <boost/corosio/detail/tcp_service.hpp>
#endif

namespace boost::corosio {

namespace {

#if BOOST_COROSIO_HAS_IOCP
using tcp_service_key = detail::win_tcp_service;
#else
using tcp_service_key = detail::tcp_service;
#endif

} // namespace

tcp_socket::~tcp_socket()
{
    close();
}

tcp_socket::tcp_socket(capy::execution_context& ctx)
    : io_object(handle(
          ctx,
          detail::use_backend_service<detail::tcp_service_of, tcp_service_key>(
              ctx)))
{
}

std::error_code
tcp_socket::open(family f) noexcept
{
    if (is_open())
        return {};
    return open_for_family(detail::native_family(f), SOCK_STREAM, IPPROTO_TCP);
}

std::error_code
tcp_socket::open_for_family(int family, int type, int protocol) noexcept
{
#if BOOST_COROSIO_HAS_IOCP
    auto& svc          = static_cast<detail::win_tcp_service&>(h_.service());
    auto& sock         = static_cast<detail::win_tcp_socket&>(*h_.get());
    std::error_code ec = svc.open_socket(sock, family, type, protocol);
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
    if (is_open())
        return make_error_code(error::already_open);
#if BOOST_COROSIO_HAS_IOCP
    auto& svc          = static_cast<detail::win_tcp_service&>(h_.service());
    auto& sock         = static_cast<detail::win_tcp_socket&>(*h_.get());
    std::error_code ec = svc.assign_socket(sock, fd);
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
    auto& svc  = static_cast<detail::win_tcp_service&>(h_.service());
    auto& sock = static_cast<detail::win_tcp_socket&>(*h_.get());
    return svc.bind_socket(sock, ep);
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
