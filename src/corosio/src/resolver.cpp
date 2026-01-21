//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/resolver.hpp>

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_IOCP)
#include "src/detail/iocp/resolver_service.hpp"
#elif defined(BOOST_COROSIO_BACKEND_EPOLL)
#include "src/detail/epoll/resolver_service.hpp"
#elif defined(BOOST_COROSIO_BACKEND_KQUEUE)
#include "src/detail/kqueue/resolver_service.hpp"
#endif

namespace boost {
namespace corosio {
namespace {

#if defined(BOOST_COROSIO_BACKEND_IOCP)
using resolver_service = detail::win_resolver_service;
using resolver_impl_type = detail::win_resolver_impl;
#elif defined(BOOST_COROSIO_BACKEND_EPOLL)
using resolver_service = detail::epoll_resolver_service;
using resolver_impl_type = detail::epoll_resolver_impl;
#elif defined(BOOST_COROSIO_BACKEND_KQUEUE)
using resolver_service = detail::kqueue_resolver_service;
using resolver_impl_type = detail::kqueue_resolver_impl;
#endif

} // namespace

resolver::
~resolver()
{
    if (impl_)
        impl_->release();
}

resolver::
resolver(
    capy::execution_context& ctx)
    : io_object(ctx)
{
    auto& svc = ctx_->use_service<resolver_service>();
    auto& impl = svc.create_impl();
    impl_ = &impl;
}

void
resolver::
cancel()
{
    if (impl_)
        static_cast<resolver_impl_type*>(impl_)->cancel();
}

} // namespace corosio
} // namespace boost
