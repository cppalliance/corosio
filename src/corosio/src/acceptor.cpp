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

#include <cassert>

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
    auto& impl = svc.create_acceptor_impl();
    impl_ = &impl;

    system::error_code ec = svc.open_acceptor(impl, ep, backlog);
    if (ec)
    {
        impl.release();
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

    impl_->release();
    impl_ = nullptr;
}

void
acceptor::
cancel()
{
    assert(impl_ != nullptr);
    static_cast<acceptor_impl_type*>(impl_)->cancel();
}

} // namespace corosio
} // namespace boost
