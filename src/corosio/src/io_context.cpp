//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/backend.hpp>

#include <thread>

#if BOOST_COROSIO_HAS_EPOLL
#include <boost/corosio/native/detail/epoll/epoll_scheduler.hpp>
#include <boost/corosio/native/detail/epoll/epoll_tcp_service.hpp>
#include <boost/corosio/native/detail/epoll/epoll_tcp_acceptor_service.hpp>
#include <boost/corosio/native/detail/epoll/epoll_udp_service.hpp>
#endif

#if BOOST_COROSIO_HAS_SELECT
#include <boost/corosio/native/detail/select/select_scheduler.hpp>
#include <boost/corosio/native/detail/select/select_tcp_service.hpp>
#include <boost/corosio/native/detail/select/select_tcp_acceptor_service.hpp>
#include <boost/corosio/native/detail/select/select_udp_service.hpp>
#endif

#if BOOST_COROSIO_HAS_KQUEUE
#include <boost/corosio/native/detail/kqueue/kqueue_scheduler.hpp>
#include <boost/corosio/native/detail/kqueue/kqueue_tcp_service.hpp>
#include <boost/corosio/native/detail/kqueue/kqueue_tcp_acceptor_service.hpp>
#include <boost/corosio/native/detail/kqueue/kqueue_udp_service.hpp>
#endif

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/native/detail/iocp/win_acceptor_service.hpp>
#include <boost/corosio/native/detail/iocp/win_signals.hpp>
#endif

namespace boost::corosio {

#if BOOST_COROSIO_HAS_EPOLL
detail::scheduler&
epoll_t::construct(capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<detail::epoll_scheduler>(
        static_cast<int>(concurrency_hint));

    ctx.make_service<detail::epoll_tcp_service>();
    ctx.make_service<detail::epoll_tcp_acceptor_service>();
    ctx.make_service<detail::epoll_udp_service>();

    return sched;
}
#endif

#if BOOST_COROSIO_HAS_SELECT
detail::scheduler&
select_t::construct(capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<detail::select_scheduler>(
        static_cast<int>(concurrency_hint));

    ctx.make_service<detail::select_tcp_service>();
    ctx.make_service<detail::select_tcp_acceptor_service>();
    ctx.make_service<detail::select_udp_service>();

    return sched;
}
#endif

#if BOOST_COROSIO_HAS_KQUEUE
detail::scheduler&
kqueue_t::construct(capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<detail::kqueue_scheduler>(
        static_cast<int>(concurrency_hint));

    ctx.make_service<detail::kqueue_tcp_service>();
    ctx.make_service<detail::kqueue_tcp_acceptor_service>();
    ctx.make_service<detail::kqueue_udp_service>();

    return sched;
}
#endif

#if BOOST_COROSIO_HAS_IOCP
detail::scheduler&
iocp_t::construct(capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<detail::win_scheduler>(
        static_cast<int>(concurrency_hint));

    auto& sockets = ctx.make_service<detail::win_sockets>();
    ctx.make_service<detail::win_acceptor_service>(sockets);
    ctx.make_service<detail::win_signals>();

    return sched;
}
#endif

io_context::io_context() : io_context(std::thread::hardware_concurrency()) {}

io_context::io_context(unsigned concurrency_hint)
    : capy::execution_context(this)
    , sched_(nullptr)
{
#if BOOST_COROSIO_HAS_IOCP
    sched_ = &iocp_t::construct(*this, concurrency_hint);
#elif BOOST_COROSIO_HAS_EPOLL
    sched_ = &epoll_t::construct(*this, concurrency_hint);
#elif BOOST_COROSIO_HAS_KQUEUE
    sched_ = &kqueue_t::construct(*this, concurrency_hint);
#elif BOOST_COROSIO_HAS_SELECT
    sched_ = &select_t::construct(*this, concurrency_hint);
#endif
}

io_context::~io_context()
{
    shutdown();
    destroy();
}

} // namespace boost::corosio
