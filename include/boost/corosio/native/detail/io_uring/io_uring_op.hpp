//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_OP_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_OP_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IO_URING

#include <boost/corosio/native/detail/coro_op.hpp>

// Forward declare to avoid circular include with io_uring_scheduler.hpp.
namespace boost::corosio::detail { class io_uring_scheduler; }

#include <atomic>

#include <liburing.h>

namespace boost::corosio::detail {

/** io_uring operation: the shared proactor op envelope plus the
    io_uring-specific completion plumbing.

    `coro_op` supplies the fields common to both proactor backends
    (coroutine handle, executor, output pointers, stop_token wiring,
    impl_ptr keepalive). This type adds the CQE result (`res`/`cqe_flags`),
    the ring-cancel visibility flag (`sqe_set`), and the two function
    pointers the run loop uses to prep an SQE and dispatch a CQE without
    template instantiation.
*/
struct io_uring_op : coro_op
{
    /// CQE-side dispatcher type. Called once per completion event.
    /// Pushes self into `local` rather than dispatching inline so
    /// process_completions can splice the batch into completed_ops_
    /// atomically and do_one dispatches one handler at a time.
    using cqe_func_type =
        void (*)(io_uring_op*, int res, unsigned flags, op_queue& local) noexcept;

    /// SQE-preparation dispatcher type. Called by the leader during
    /// its drain step to fill an SQE for this op. Concrete op types
    /// set this at construction so the new submit path is purely
    /// data-driven (no template instantiation, no allocation).
    using prep_func_type =
        void (*)(io_uring_op*, ::io_uring_sqe*) noexcept;

    explicit io_uring_op(
        func_type      post_func,
        cqe_func_type  cqe_fn,
        prep_func_type prep_fn = nullptr) noexcept
        : coro_op(post_func)
        , cqe_func(cqe_fn)
        , prep_func(prep_fn)
    {}

    int                                          res       = 0;
    unsigned                                     cqe_flags = 0;
    /// True after `io_uring_sqe_set_data` has linked an SQE to this op.
    /// Until then, on_cancel() has nothing for the kernel to find.
    std::atomic<bool>                            sqe_set{false};
    cqe_func_type                                cqe_func;
    /// SQE-preparation dispatcher. nullptr for ops still using the
    /// old `io_uring_submit_op<PrepFn>(prep)` template path
    /// (UDP/local/file/dgram during plan 5a). Set non-null by ops
    /// migrated to the queue-based submit path.
    prep_func_type                               prep_func;

    /// Scheduler reference for submitting cancel SQEs on stop_token.
    io_uring_scheduler*                          sched_ = nullptr;

    /// Bridge virtual dispatch to func-pointer dispatch. Lets the run
    /// loop dispatch any scheduler_op via `(*op)()` — both reactor-style
    /// services posted into the queue and proactor-style io_uring ops.
    /// `owner` is non-null per scheduler_op's completion-vs-destroy
    /// convention (see scheduler_op.hpp).
    void operator()() override { complete(this, 0, 0); }

    /// Arm the stop-token callback. Must be called before the SQE submits.
    /// Extends coro_op::start to also clear the ring-cancel flag.
    void start(std::stop_token const& token)
    {
        sqe_set.store(false, std::memory_order_relaxed);
        coro_op::start(token);
    }

    /// io_uring cancellation: record the request and, if an SQE was already
    /// linked, ask the kernel to cancel it by user_data.
    void on_cancel() noexcept override;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IO_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_OP_HPP
