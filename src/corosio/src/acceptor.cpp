//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/acceptor.hpp>

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_IOCP)
#include "src/detail/iocp/sockets.hpp"
#elif defined(BOOST_COROSIO_BACKEND_EPOLL)
#include "src/detail/epoll/sockets.hpp"
#endif

#include <boost/corosio/detail/except.hpp>

namespace boost {
namespace corosio {
namespace {

#if defined(BOOST_COROSIO_BACKEND_IOCP)
using acceptor_service = detail::win_sockets;
using acceptor_impl_type = detail::win_acceptor_impl;
#elif defined(BOOST_COROSIO_BACKEND_EPOLL)
using acceptor_service = detail::epoll_sockets;
using acceptor_impl_type = detail::epoll_acceptor_impl;
#endif

} // namespace

acceptor::
~acceptor()
{
    close();
}

acceptor::
acceptor(
    capy::execution_context& ctx)
    : io_object(ctx)
{
}

void
acceptor::
listen(endpoint ep, int backlog)
{
    if (impl_)
        close();

    auto& svc = ctx_->use_service<acceptor_service>();
    auto& wrapper = svc.create_acceptor_impl();
    impl_ = &wrapper;

#if defined(BOOST_COROSIO_BACKEND_IOCP)
    system::error_code ec = svc.open_acceptor(
        *wrapper.get_internal(), ep, backlog);
#elif defined(BOOST_COROSIO_BACKEND_EPOLL)
    system::error_code ec = svc.open_acceptor(wrapper, ep, backlog);
#endif
    if (ec)
    {
        wrapper.release();
        impl_ = nullptr;
        detail::throw_system_error(ec, "acceptor::listen");
    }
}

void
acceptor::
close()
{
    if (!impl_)
        return;

    auto* wrapper = static_cast<acceptor_impl_type*>(impl_);
    wrapper->release();
    impl_ = nullptr;
}

void
acceptor::
cancel()
{
    if (!impl_)
        return;
#if defined(BOOST_COROSIO_BACKEND_IOCP)
    static_cast<acceptor_impl_type*>(impl_)->get_internal()->cancel();
#elif defined(BOOST_COROSIO_BACKEND_EPOLL)
    static_cast<acceptor_impl_type*>(impl_)->cancel();
#endif
}

endpoint
acceptor::
local_endpoint() const noexcept
{
    if (!impl_)
        return endpoint{};
    return get().local_endpoint();
}

} // namespace corosio
} // namespace boost
