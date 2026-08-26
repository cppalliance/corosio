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
    [[maybe_unused]] capy::execution_context& ctx,
    [[maybe_unused]] io_context_options const& opts)
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

}

// Map the locking tier to the scheduler's threading facilities. one_thread is
// set only for the lockless tiers, where a single run thread is guaranteed.
detail::scheduler::threading_config
make_threading_config(io_context_options const& opts)
{
    detail::scheduler::threading_config cfg;
    cfg.scheduler_locking  = opts.locking != locking_mode::unsafe;
    cfg.reactor_io_locking = opts.locking == locking_mode::safe;
    cfg.one_thread         = opts.locking != locking_mode::safe;
    return cfg;
}

// Apply runtime tuning after construction. `concurrency_hint` is the effective
// hint (normalized to 1 for lockless tiers). Budget heuristic: with default
// budgets and hint > 1, disable the inline-completion fast path so multi-thread
// runs post everything for cross-thread work-stealing.
void
apply_scheduler_options(
    [[maybe_unused]] detail::scheduler& sched,
    [[maybe_unused]] io_context_options const& opts,
    [[maybe_unused]] unsigned concurrency_hint)
{
    sched.configure_threading(make_threading_config(opts));

#if BOOST_COROSIO_HAS_EPOLL || BOOST_COROSIO_HAS_KQUEUE || BOOST_COROSIO_HAS_SELECT
    // dynamic_cast — when io_uring is also linked, the runtime probe may
    // have selected io_uring_scheduler instead of a reactor_scheduler.
    if (auto* reactor =
            dynamic_cast<detail::reactor_scheduler*>(&sched))
    {
        // Detect "user kept the defaults" by comparing all three to the
        // io_context-options-defined struct defaults.
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
    }
#endif

#if BOOST_COROSIO_HAS_IO_URING
    if (auto* uring_sched =
            dynamic_cast<detail::io_uring_scheduler*>(&sched))
    {
        if (opts.enable_sqpoll)
            uring_sched->configure_sqpoll(
                true, opts.sq_thread_idle_ms, opts.sq_thread_cpu);
    }
#endif

}

// Bring up backend infrastructure whose setup depends on the options
// applied above. Runs last in every constructor: an io_context that
// constructs is usable, so a kernel that refuses the infrastructure is
// reported from the constructor and not from the first operation.
void
finish_construction([[maybe_unused]] detail::scheduler& sched)
{
#if BOOST_COROSIO_HAS_IO_URING
    if (auto* uring_sched =
            dynamic_cast<detail::io_uring_scheduler*>(&sched))
        uring_sched->init_ring();
#endif
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

io_context::io_context()
    : io_context(std::max(1u, std::thread::hardware_concurrency()))
{
}

io_context::io_context(unsigned concurrency_hint)
    : capy::execution_context(this)
    , sched_(&construct_default(*this, concurrency_hint))
{
    // Threading config only; the plain path leaves the reactor budget at its
    // defaults (no options-ctor budget heuristic).
    apply_threading_(io_context_options{});
}

io_context::io_context(
    io_context_options const& opts_in,
    unsigned concurrency_hint)
    : capy::execution_context(this)
    , sched_(nullptr)
{
    apply_options_pre_(opts_in);
    // Computed before construct_default so IOCP's completion port is created
    // with the effective concurrency.
    unsigned const eff =
        detail::effective_concurrency_hint(opts_in, concurrency_hint);
    sched_ = &construct_default(*this, eff);
    apply_options_post_(opts_in, eff);
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
    apply_scheduler_options(*sched_, opts_in, concurrency_hint);
    finish_construction(*sched_);
}

void
io_context::apply_threading_(io_context_options const& opts_in)
{
    sched_->configure_threading(make_threading_config(opts_in));
    finish_construction(*sched_);
}

io_context::~io_context()
{
    shutdown();
    destroy();
}

} // namespace boost::corosio
