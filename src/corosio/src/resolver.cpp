//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/resolver.hpp>


#if defined(_WIN32)
#include "src/detail/iocp/resolver_service.hpp"
#else
#include "src/detail/posix/resolver_service.hpp"
#endif

#include <stdexcept>

/*
    Resolver Frontend
    =================

    This file implements the public resolver class, which delegates to
    platform-specific services:
    - Windows: win_resolver_service (uses GetAddrInfoExW)
    - POSIX: posix_resolver_service (uses getaddrinfo + worker threads)

    The resolver constructor uses find_service() to locate the resolver
    service, which must have been previously created by the scheduler
    during io_context construction. If not found, construction fails.

    This separation allows the public API to be platform-agnostic while
    the implementation details are hidden in the detail namespace.
*/

namespace boost::corosio {
namespace {

#if defined(_WIN32)
using resolver_service = detail::win_resolver_service;
#else
using resolver_service = detail::posix_resolver_service;
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
    auto* svc = ctx_->find_service<resolver_service>();
    if (!svc)
    {
        // Resolver service not yet created - this happens if io_context
        // hasn't been constructed yet, or if the scheduler didn't
        // initialize the resolver service
        throw std::runtime_error("resolver_service not found");
    }
    auto& impl = svc->create_impl();
    impl_ = &impl;
}

void
resolver::
cancel()
{
    if (impl_)
        get().cancel();
}

} // namespace boost::corosio
