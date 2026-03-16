//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_tcp_acceptor_service.hpp>
#else
#include <boost/corosio/detail/tcp_acceptor_service.hpp>
#endif

#include <boost/corosio/detail/except.hpp>

namespace boost::corosio {

tcp_acceptor::~tcp_acceptor()
{
    close();
}

tcp_acceptor::tcp_acceptor(capy::execution_context& ctx)
#if BOOST_COROSIO_HAS_IOCP
    : io_object(create_handle<detail::win_tcp_acceptor_service>(ctx))
#else
    : io_object(create_handle<detail::tcp_acceptor_service>(ctx))
#endif
{
}

tcp_acceptor::tcp_acceptor(
    capy::execution_context& ctx, endpoint ep, int backlog)
    : tcp_acceptor(ctx)
{
    open(ep.is_v6() ? tcp::v6() : tcp::v4());
    set_option(socket_option::reuse_address(true));
    if (auto ec = bind(ep))
        detail::throw_system_error(ec, "tcp_acceptor");
    if (auto ec = listen(backlog))
        detail::throw_system_error(ec, "tcp_acceptor");
}

void
tcp_acceptor::open(tcp proto)
{
    if (is_open())
        return;

#if BOOST_COROSIO_HAS_IOCP
    auto& svc = static_cast<detail::win_tcp_acceptor_service&>(h_.service());
#else
    auto& svc = static_cast<detail::tcp_acceptor_service&>(h_.service());
#endif
    std::error_code ec = svc.open_acceptor_socket(
        *static_cast<tcp_acceptor::implementation*>(h_.get()), proto.family(),
        proto.type(), proto.protocol());
    if (ec)
        detail::throw_system_error(ec, "tcp_acceptor::open");
}

std::error_code
tcp_acceptor::bind(endpoint ep)
{
    if (!is_open())
        detail::throw_logic_error("bind: acceptor not open");
#if BOOST_COROSIO_HAS_IOCP
    auto& svc = static_cast<detail::win_tcp_acceptor_service&>(h_.service());
#else
    auto& svc = static_cast<detail::tcp_acceptor_service&>(h_.service());
#endif
    return svc.bind_acceptor(
        *static_cast<tcp_acceptor::implementation*>(h_.get()), ep);
}

std::error_code
tcp_acceptor::listen(int backlog)
{
    if (!is_open())
        detail::throw_logic_error("listen: acceptor not open");
#if BOOST_COROSIO_HAS_IOCP
    auto& svc = static_cast<detail::win_tcp_acceptor_service&>(h_.service());
#else
    auto& svc = static_cast<detail::tcp_acceptor_service&>(h_.service());
#endif
    return svc.listen_acceptor(
        *static_cast<tcp_acceptor::implementation*>(h_.get()), backlog);
}

void
tcp_acceptor::close()
{
    if (!is_open())
        return;
    h_.service().close(h_);
}

void
tcp_acceptor::cancel()
{
    if (!is_open())
        return;
    get().cancel();
}

endpoint
tcp_acceptor::local_endpoint() const noexcept
{
    if (!is_open())
        return endpoint{};
    return get().local_endpoint();
}

} // namespace boost::corosio
