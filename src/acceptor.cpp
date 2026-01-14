//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/acceptor.hpp>

#ifdef _WIN32
#include "detail/win_iocp_sockets.hpp"
#else
#include "detail/posix_sockets.hpp"
#endif

#include <boost/corosio/detail/except.hpp>

#include <cassert>

namespace boost {
namespace corosio {
namespace {

#ifdef _WIN32
using acceptor_service = detail::win_iocp_sockets;
using acceptor_impl_type = detail::win_acceptor_impl;
#else
using acceptor_service = detail::posix_sockets;
using acceptor_impl_type = detail::posix_acceptor_impl;
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
