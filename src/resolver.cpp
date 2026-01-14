//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/resolver.hpp>

#ifdef _WIN32
#include "detail/win_iocp_resolver_service.hpp"
#else
#include "detail/posix_resolver_service.hpp"
#endif

namespace boost {
namespace corosio {
namespace {

#ifdef _WIN32
using resolver_service = detail::win_iocp_resolver_service;
using resolver_impl_type = detail::win_resolver_impl;
#else
using resolver_service = detail::posix_resolver_service;
using resolver_impl_type = detail::posix_resolver_impl;
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
