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

/**
 * @brief Ensure the socket has an underlying implementation and open the native socket handle.
 *
 * Creates and stores the backend-specific socket implementation from the execution context
 * and opens the associated native socket handle. If the socket is already open, the call
 * returns immediately.
 *
 * @throws std::system_error If the underlying service fails to open the native socket; the
 *         implementation is released before the exception is thrown.
 */
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

/**
 * @brief Releases the underlying socket implementation and resets the socket state.
 *
 * If the socket is not open, the call has no effect. Otherwise this releases the
 * held implementation (closing the underlying resource) and clears the internal
 * implementation pointer.
 */
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

/**
 * @brief Cancel all outstanding asynchronous operations on the socket.
 *
 * Requires that the socket implementation has been created (socket is open).
 * After calling this, any pending asynchronous I/O initiated on the underlying
 * socket will be requested to cancel.
 *
 * @pre impl_ must be non-null (socket must be open).
 */
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

/**
 * @brief Shuts down one or both directions of the underlying socket.
 *
 * If the socket has an active implementation, delegates the shutdown request
 * to that implementation; otherwise the call is a no-op.
 *
 * @param what Specifies which direction(s) to shut down (for example: read, write, or both).
 */
void
socket::
shutdown(shutdown_type what)
{
    if (impl_)
        get().shutdown(what);
}

} // namespace corosio
} // namespace boost