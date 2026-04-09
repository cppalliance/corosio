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
#include <boost/corosio/detail/thread_pool.hpp>

#include <stdexcept>
#include <thread>

// Reactor backend types come from backend.hpp via reactor_backend.hpp.
// Only IOCP needs additional includes.

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/native/detail/iocp/win_tcp_acceptor_service.hpp>
#include <boost/corosio/native/detail/iocp/win_udp_service.hpp>
#include <boost/corosio/native/detail/iocp/win_signals.hpp>
#include <boost/corosio/native/detail/iocp/win_file_service.hpp>
#include <boost/corosio/native/detail/iocp/win_random_access_file_service.hpp>
#endif

namespace boost::corosio {

#if BOOST_COROSIO_HAS_EPOLL
detail::scheduler&
epoll_t::construct(capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<detail::epoll_scheduler>(
        static_cast<int>(concurrency_hint));

    ctx.make_service<epoll_t::tcp_service_type>();
    ctx.make_service<epoll_t::tcp_acceptor_service_type>();
    ctx.make_service<epoll_t::udp_service_type>();
    ctx.make_service<epoll_t::local_stream_service_type>();
    ctx.make_service<epoll_t::local_stream_acceptor_service_type>();
    ctx.make_service<epoll_t::local_datagram_service_type>();

    return sched;
}
#endif

#if BOOST_COROSIO_HAS_SELECT
detail::scheduler&
select_t::construct(capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<detail::select_scheduler>(
        static_cast<int>(concurrency_hint));

    ctx.make_service<select_t::tcp_service_type>();
    ctx.make_service<select_t::tcp_acceptor_service_type>();
    ctx.make_service<select_t::udp_service_type>();
    ctx.make_service<select_t::local_stream_service_type>();
    ctx.make_service<select_t::local_stream_acceptor_service_type>();
    ctx.make_service<select_t::local_datagram_service_type>();

    return sched;
}
#endif

#if BOOST_COROSIO_HAS_KQUEUE
detail::scheduler&
kqueue_t::construct(capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<detail::kqueue_scheduler>(
        static_cast<int>(concurrency_hint));

    ctx.make_service<kqueue_t::tcp_service_type>();
    ctx.make_service<kqueue_t::tcp_acceptor_service_type>();
    ctx.make_service<kqueue_t::udp_service_type>();
    ctx.make_service<kqueue_t::local_stream_service_type>();
    ctx.make_service<kqueue_t::local_stream_acceptor_service_type>();
    ctx.make_service<kqueue_t::local_datagram_service_type>();

    return sched;
}
#endif

#if BOOST_COROSIO_HAS_IOCP
detail::scheduler&
iocp_t::construct(capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<detail::win_scheduler>(
        static_cast<int>(concurrency_hint));

    auto& tcp_svc = ctx.make_service<detail::win_tcp_service>();
    ctx.make_service<detail::win_tcp_acceptor_service>(tcp_svc);
    ctx.make_service<detail::win_udp_service>();
    ctx.make_service<detail::win_signals>();
    ctx.make_service<detail::win_file_service>();
    ctx.make_service<detail::win_random_access_file_service>();

    return sched;
}
#endif

namespace {

// Pre-create services that must exist before construct() runs.
void
pre_create_services(
    capy::execution_context& ctx,
    io_context_options const& opts)
{
#if BOOST_COROSIO_POSIX
    if (opts.thread_pool_size < 1)
        throw std::invalid_argument(
            "thread_pool_size must be at least 1");
    // Pre-create the shared thread pool with the configured size.
    // This must happen before construct() because the scheduler
    // constructor creates file and resolver services that call
    // get_or_create_pool(), which would create a 1-thread pool.
    if (opts.thread_pool_size != 1)
        ctx.make_service<detail::thread_pool>(opts.thread_pool_size);
#endif

    (void)ctx;
    (void)opts;
}

// Apply runtime tuning to the scheduler after construction.
void
apply_scheduler_options(
    detail::scheduler& sched,
    io_context_options const& opts)
{
#if BOOST_COROSIO_HAS_EPOLL || BOOST_COROSIO_HAS_KQUEUE || BOOST_COROSIO_HAS_SELECT
    auto& reactor =
        static_cast<detail::reactor_scheduler&>(sched);
    reactor.configure_reactor(
        opts.max_events_per_poll,
        opts.inline_budget_initial,
        opts.inline_budget_max,
        opts.unassisted_budget);
    if (opts.single_threaded)
        reactor.configure_single_threaded(true);
#endif

#if BOOST_COROSIO_HAS_IOCP
    auto& iocp_sched = static_cast<detail::win_scheduler&>(sched);
    iocp_sched.configure_iocp(opts.gqcs_timeout_ms);
    if (opts.single_threaded)
        iocp_sched.configure_single_threaded(true);
#endif

    (void)sched;
    (void)opts;
}

detail::scheduler&
construct_default(capy::execution_context& ctx, unsigned concurrency_hint)
{
#if BOOST_COROSIO_HAS_IOCP
    return iocp_t::construct(ctx, concurrency_hint);
#elif BOOST_COROSIO_HAS_EPOLL
    return epoll_t::construct(ctx, concurrency_hint);
#elif BOOST_COROSIO_HAS_KQUEUE
    return kqueue_t::construct(ctx, concurrency_hint);
#elif BOOST_COROSIO_HAS_SELECT
    return select_t::construct(ctx, concurrency_hint);
#endif
}

} // anonymous namespace

io_context::io_context() : io_context(std::thread::hardware_concurrency()) {}

io_context::io_context(unsigned concurrency_hint)
    : capy::execution_context(this)
    , sched_(&construct_default(*this, concurrency_hint))
{
}

io_context::io_context(
    io_context_options const& opts,
    unsigned concurrency_hint)
    : capy::execution_context(this)
    , sched_(nullptr)
{
    pre_create_services(*this, opts);
    sched_ = &construct_default(*this, concurrency_hint);
    apply_scheduler_options(*sched_, opts);
}

void
io_context::apply_options_pre_(io_context_options const& opts)
{
    pre_create_services(*this, opts);
}

void
io_context::apply_options_post_(io_context_options const& opts)
{
    apply_scheduler_options(*sched_, opts);
}

io_context::~io_context()
{
    shutdown();
    destroy();
}

} // namespace boost::corosio
