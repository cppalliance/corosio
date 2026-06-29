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

#include <algorithm>
#include <stdexcept>
#include <thread>

#if BOOST_COROSIO_HAS_EPOLL
#include <boost/corosio/native/detail/epoll/epoll_types.hpp>
#endif

#if BOOST_COROSIO_HAS_SELECT
#include <boost/corosio/native/detail/select/select_types.hpp>
#endif

#if BOOST_COROSIO_HAS_KQUEUE
#include <boost/corosio/native/detail/kqueue/kqueue_types.hpp>
#endif

#if BOOST_COROSIO_HAS_IO_URING
#include <boost/corosio/native/detail/io_uring/io_uring_acceptor_ops.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_buffer.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_dgram_ops.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_multishot_acceptor.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_random_access_file.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_scheduler.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_stream_file.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_types.hpp>
#endif

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/native/detail/iocp/win_tcp_acceptor_service.hpp>
#include <boost/corosio/native/detail/iocp/win_udp_service.hpp>
#include <boost/corosio/native/detail/iocp/win_local_stream_acceptor_service.hpp>
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

    ctx.make_service<detail::epoll_tcp_service>();
    ctx.make_service<detail::epoll_tcp_acceptor_service>();
    ctx.make_service<detail::epoll_udp_service>();
    ctx.make_service<detail::epoll_local_stream_service>();
    ctx.make_service<detail::epoll_local_stream_acceptor_service>();
    ctx.make_service<detail::epoll_local_datagram_service>();

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
    ctx.make_service<detail::select_local_stream_service>();
    ctx.make_service<detail::select_local_stream_acceptor_service>();
    ctx.make_service<detail::select_local_datagram_service>();

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
    ctx.make_service<detail::kqueue_local_stream_service>();
    ctx.make_service<detail::kqueue_local_stream_acceptor_service>();
    ctx.make_service<detail::kqueue_local_datagram_service>();

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
    auto& local_svc =
        ctx.make_service<detail::win_local_stream_service>(tcp_svc);
    ctx.make_service<detail::win_local_stream_acceptor_service>(local_svc);
    ctx.make_service<detail::win_signals>();
    ctx.make_service<detail::win_file_service>();
    ctx.make_service<detail::win_random_access_file_service>();

    return sched;
}
#endif

#if BOOST_COROSIO_HAS_IO_URING
detail::scheduler&
io_uring_t::construct(capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<detail::io_uring_scheduler>(
        static_cast<int>(concurrency_hint));

    ctx.make_service<detail::io_uring_tcp_service>();
    ctx.make_service<detail::io_uring_tcp_acceptor_service>();
    ctx.make_service<detail::io_uring_local_stream_service>();
    ctx.make_service<detail::io_uring_local_stream_acceptor_service>();
    ctx.make_service<detail::io_uring_udp_service>();
    ctx.make_service<detail::io_uring_local_datagram_service>();
    ctx.make_service<detail::io_uring_stream_file_service>(sched);
    ctx.make_service<detail::io_uring_random_access_file_service>(sched);

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
//
// Concurrency-hint heuristic for budget defaults: when the io_context is
// constructed with concurrency_hint > 1 AND the user has not customized
// the budget settings (i.e. they remain at the struct defaults), we
// disable the inline-completion fast path. Multi-thread workloads
// benefit from "always-post" because cross-thread work-stealing wins
// over chained dispatch on the originating thread. Single-thread (or
// any custom budget) keeps the user/library setting unchanged.
void
apply_scheduler_options(
    detail::scheduler& sched,
    io_context_options const& opts,
    unsigned concurrency_hint)
{
#if BOOST_COROSIO_HAS_EPOLL || BOOST_COROSIO_HAS_KQUEUE || BOOST_COROSIO_HAS_SELECT
    // dynamic_cast — when io_uring is also linked, the runtime probe may
    // have selected io_uring_scheduler instead of a reactor_scheduler.
    if (auto* reactor =
            dynamic_cast<detail::reactor_scheduler*>(&sched))
    {
        // Detect "user kept the defaults" by comparing all three to the
        // io_context_options-defined struct defaults.
        io_context_options defaults;
        bool budget_at_defaults =
            opts.inline_budget_initial == defaults.inline_budget_initial &&
            opts.inline_budget_max == defaults.inline_budget_max &&
            opts.unassisted_budget == defaults.unassisted_budget;

        unsigned init = opts.inline_budget_initial;
        unsigned max  = opts.inline_budget_max;
        unsigned ua   = opts.unassisted_budget;

        if (budget_at_defaults && concurrency_hint > 1)
        {
            // Multi-thread default: disable budget (post-everything).
            init = 0;
            max  = 0;
            ua   = 0;
        }

        reactor->configure_reactor(
            opts.max_events_per_poll,
            init,
            max,
            ua);
        if (opts.single_threaded)
            reactor->configure_single_threaded(true);
    }
#endif

#if BOOST_COROSIO_HAS_IO_URING
    if (auto* uring_sched =
            dynamic_cast<detail::io_uring_scheduler*>(&sched))
    {
        if (opts.single_threaded)
            uring_sched->configure_single_threaded(true);
        if (opts.enable_sqpoll)
            uring_sched->configure_sqpoll(
                true, opts.sq_thread_idle_ms, opts.sq_thread_cpu);
    }
#endif

#if BOOST_COROSIO_HAS_IOCP
    auto& iocp_sched = static_cast<detail::win_scheduler&>(sched);
    if (opts.single_threaded)
        iocp_sched.configure_single_threaded(true);
#endif

    (void)sched;
    (void)opts;
    (void)concurrency_hint;
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

// Tie concurrency_hint == 1 to single_threaded (asio precedent).
io_context_options
normalize_options(io_context_options opts, unsigned concurrency_hint)
{
    if (concurrency_hint == 1)
        opts.single_threaded = true;
    return opts;
}

} // anonymous namespace

io_context::io_context()
    : io_context(std::max(2u, std::thread::hardware_concurrency()))
{
}

io_context::io_context(unsigned concurrency_hint)
    : capy::execution_context(this)
    , sched_(&construct_default(*this, concurrency_hint))
{
    if (concurrency_hint == 1)
        configure_single_threaded_();
}

io_context::io_context(
    io_context_options const& opts_in,
    unsigned concurrency_hint)
    : capy::execution_context(this)
    , sched_(nullptr)
{
    auto opts = normalize_options(opts_in, concurrency_hint);
    pre_create_services(*this, opts);
    sched_ = &construct_default(*this, concurrency_hint);
    apply_scheduler_options(*sched_, opts, concurrency_hint);
}

void
io_context::apply_options_pre_(io_context_options const& opts)
{
    pre_create_services(*this, opts);
}

void
io_context::apply_options_post_(
    io_context_options const& opts_in,
    unsigned concurrency_hint)
{
    auto opts = normalize_options(opts_in, concurrency_hint);
    apply_scheduler_options(*sched_, opts, concurrency_hint);
}

void
io_context::configure_single_threaded_()
{
    // Dispatched through the scheduler base's virtual override; avoids
    // unsafe downcasts when the active backend is io_uring rather than
    // reactor (on Linux both BOOST_COROSIO_HAS_EPOLL and the io_uring
    // backend may be enabled simultaneously).
    sched_->configure_single_threaded(true);
}

io_context::~io_context()
{
    shutdown();
    destroy();
}

} // namespace boost::corosio
