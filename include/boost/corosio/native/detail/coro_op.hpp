//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_CORO_OP_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_CORO_OP_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/continuation_op.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/capy/ex/executor_ref.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <system_error>

/*
    Shared, non-template op envelope for every native backend — the readiness
    reactors (epoll/kqueue/select), io_uring, and IOCP. It captures the part of
    an async operation that is identical regardless of how completion is
    reported: the coroutine to resume, the executor it dispatches on, the
    output pointers, the stop_token wiring, the cancelled flag, and the
    keepalive that holds the owning impl alive while the op is in flight.

    What is deliberately NOT here (it differs by backend and stays in the
    derived op layer):
      - the result model: the reactors re-run the syscall and record
        `errn`/`bytes_transferred` (reactor_op_base); io_uring stores the raw
        `res`/`cqe_flags`; IOCP stores `dwError`/`bytes_transferred`. Each
        decodes its own result.
      - the submission + the kernel cancel action. Cancellation is unified only
        at the call site via the virtual `on_cancel()` hook: the stop_callback
        always targets `coro_op`, and each backend overrides `on_cancel()` —
        the reactors route to the owning impl's cancel(), io_uring submits an
        ASYNC_CANCEL SQE, IOCP calls the stored cancel_func_/CancelIoEx.

    See tasks/proactor-dedup-decisions.md and coro-op-unification-scope.md.
*/

namespace boost::corosio::detail {

/** Non-template op envelope shared by every native backend's operations.

    `reactor_op_base`, `io_uring_op`, and `overlapped_op` all derive from this.
    Derives from scheduler_op so ops queue intrusively and dispatch through the
    function-pointer (io_uring/IOCP) or virtual (reactors) completion path —
    hence both a default and a func_type constructor.

    @note For IOCP, the concrete op multiply-inherits `OVERLAPPED` as its
    first base (so `static_cast<OVERLAPPED*>` round-trips); `coro_op`
    follows it.
*/
struct coro_op : scheduler_op
{
    /** Stop-callback handler: routes a stop_token firing to `on_cancel()`.

        A single canceller type for both backends keeps `stop_cb` (and thus
        `start()`) in this shared base; the backend-specific action lives
        behind the `on_cancel()` virtual.
    */
    struct canceller
    {
        coro_op* op;
        void operator()() const noexcept { op->on_cancel(); }
    };

    std::coroutine_handle<>  h;
    detail::continuation_op  cont_op;
    capy::executor_ref       ex;
    std::error_code*         ec_out    = nullptr;
    std::size_t*             bytes_out = nullptr;

    /// True for receive/read ops (drives the zero-byte == EOF decision).
    bool                     is_read      = false;
    /// True when the submitted buffer was zero-length (suppresses EOF).
    bool                     empty_buffer = false;

    std::atomic<bool>                            cancelled{false};
    std::optional<std::stop_callback<canceller>> stop_cb;

    /// Keeps the owning impl alive while the op is in flight (the kernel
    /// owns user buffers until completion). Dropped in the handler's resume
    /// tail (see coro_op_complete.hpp).
    std::shared_ptr<void>    impl_ptr;

    /// Default-construct for virtual-dispatch backends (the reactors, which
    /// override operator()/destroy() and leave func_ null).
    coro_op() noexcept = default;

    /// Construct with the completion function for func-pointer dispatch
    /// (io_uring / IOCP completion handlers).
    explicit coro_op(func_type func) noexcept : scheduler_op(func) {}

    /** Arm the stop-token callback. Call before the op is submitted.

        Resets the cancellation flag and (re)arms `stop_cb` against @a token.
        Derived ops that carry extra pre-submit state (e.g. io_uring's
        `sqe_set`) extend this.
    */
    void start(std::stop_token const& token)
    {
        cancelled.store(false, std::memory_order_relaxed);
        stop_cb.reset();
        if (token.stop_possible())
            stop_cb.emplace(token, canceller{this});
    }

    /// Mark this op cancellation-requested. Shared by every backend.
    void request_cancel() noexcept
    {
        cancelled.store(true, std::memory_order_release);
    }

    /** Backend cancellation hook, invoked when the stop_token fires.

        The default just records the request. Backends override to also
        drive the kernel: io_uring submits an ASYNC_CANCEL SQE; IOCP calls
        its stored cancel_func_ (CancelIoEx / wait-reactor deregister).
    */
    virtual void on_cancel() noexcept { request_cancel(); }
};

} // namespace boost::corosio::detail

#endif
