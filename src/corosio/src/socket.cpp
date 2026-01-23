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
        return;

    auto& svc = ctx_->use_service<socket_service>();
    auto& wrapper = svc.create_impl();
    impl_ = &wrapper;

#if defined(BOOST_COROSIO_BACKEND_IOCP)
    system::error_code ec = svc.open_socket(*wrapper.get_internal());
#elif defined(BOOST_COROSIO_BACKEND_EPOLL)
    system::error_code ec = svc.open_socket(wrapper);
#endif
    if (ec)
    {
        wrapper.release();
        impl_ = nullptr;
        detail::throw_system_error(ec, "socket::open");
    }
}

void
socket::
close()
{
    if (!impl_)
        return;

    auto* wrapper = static_cast<socket_impl_type*>(impl_);
    wrapper->release();
    impl_ = nullptr;
}

void
socket::
cancel()
{
    assert(impl_ != nullptr);
#if defined(BOOST_COROSIO_BACKEND_IOCP)
    static_cast<socket_impl_type*>(impl_)->get_internal()->cancel();
#elif defined(BOOST_COROSIO_BACKEND_EPOLL)
    static_cast<socket_impl_type*>(impl_)->cancel();
#endif
}

void
socket::
shutdown(shutdown_type what)
{
    if (impl_)
        get().shutdown(what);
}

native_handle_type
socket::
native_handle() const noexcept
{
    if (!impl_)
    {
#if defined(BOOST_COROSIO_BACKEND_IOCP)
        return static_cast<native_handle_type>(~0ull);  // INVALID_SOCKET
#else
        return -1;
#endif
    }
    return get().native_handle();
}

} // namespace corosio
} // namespace boost
