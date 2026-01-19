//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/socket.hpp>
#include <boost/corosio/detail/except.hpp>

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_IOCP)
#include "src/detail/iocp/sockets.hpp"
#elif defined(BOOST_COROSIO_BACKEND_EPOLL)
#include "src/detail/epoll/sockets.hpp"
#endif

#include <cassert>

namespace boost {
namespace corosio {

namespace {
#if defined(BOOST_COROSIO_BACKEND_IOCP)
using socket_service = detail::win_sockets;
using socket_impl_type = detail::win_socket_impl;
#elif defined(BOOST_COROSIO_BACKEND_EPOLL)
using socket_service = detail::epoll_sockets;
using socket_impl_type = detail::epoll_socket_impl;
#endif
} // namespace

socket::
~socket()
{
    close();
}

socket::
socket(
    capy::execution_context& ctx)
    : io_stream(ctx)
{
}

void
socket::
open()
{
    if (impl_)
        return; // Already open

    auto& svc = ctx_->use_service<socket_service>();
    auto& impl = svc.create_impl();
    impl_ = &impl;

    system::error_code ec = svc.open_socket(impl);
    if (ec)
    {
        impl.release();
        impl_ = nullptr;
        detail::throw_system_error(ec, "socket::open");
    }
}

void
socket::
close()
{
    if (!impl_)
        return; // Already closed

    impl_->release();
    impl_ = nullptr;
}

void
socket::
cancel()
{
    assert(impl_ != nullptr);
    static_cast<socket_impl_type*>(impl_)->cancel();
}

} // namespace corosio
} // namespace boost
