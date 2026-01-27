//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/acceptor.hpp>


#if defined(_WIN32)
#include "src/detail/iocp/sockets.hpp"
#else
// POSIX backends use the abstract acceptor_service interface
#include "src/detail/socket_service.hpp"
#endif

#include <boost/corosio/detail/except.hpp>

namespace boost::corosio {

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

#if defined(_WIN32)
    auto& svc = ctx_->use_service<detail::win_sockets>();
    auto& wrapper = svc.create_acceptor_impl();
    impl_ = &wrapper;
    system::error_code ec = svc.open_acceptor(
        *wrapper.get_internal(), ep, backlog);
#else
    // POSIX backends use abstract acceptor_service for runtime polymorphism.
    // The concrete service (epoll_sockets or select_sockets) must be installed
    // by the context constructor before any acceptor operations.
    auto* svc = ctx_->find_service<detail::acceptor_service>();
    if (!svc)
        detail::throw_logic_error("acceptor::listen: no acceptor service installed");
    auto& wrapper = svc->create_acceptor_impl();
    impl_ = &wrapper;
    system::error_code ec = svc->open_acceptor(wrapper, ep, backlog);
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

    // acceptor_impl has virtual release() method
    impl_->release();
    impl_ = nullptr;
}

void
acceptor::
cancel()
{
    if (!impl_)
        return;
#if defined(_WIN32)
    static_cast<detail::win_acceptor_impl*>(impl_)->get_internal()->cancel();
#else
    // acceptor_impl has virtual cancel() method
    get().cancel();
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

} // namespace boost::corosio
