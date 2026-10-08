//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_DISPATCH_CORO_HPP
#define BOOST_COROSIO_DETAIL_DISPATCH_CORO_HPP

#include <boost/corosio/io_context.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/any_executor.hpp>
#include <boost/capy/ex/frame_alloc_mixin.hpp>
#include <boost/capy/detail/type_id.hpp>
#include <coroutine>
#include <typeinfo>

namespace boost::corosio::detail {

/** Trampoline frame that lends an executor a stable continuation.

    An executor's queue holds a dispatched continuation by reference
    until dequeue (its stable-address contract) — storage a
    completion about to recycle, reuse, or delete its op cannot
    provide. The trampoline's frame owns the continuation for exactly
    the queue-residency window; when the executor resumes it, it
    resumes the real handle and self-destroys.

    run_async cannot serve here: its launch wrapper calls
    `ex.on_work_finished()` after the task completes, but a
    completion's `executor_ref` points into the awaiting coroutine's
    environment, which the resume destroys — completions must never
    touch the executor after resuming the handle. This trampoline
    touches nothing after the resume (same accounting as the plain
    `ex.dispatch` it replaces).
*/
struct resume_trampoline
{
    struct promise_type : capy::frame_alloc_mixin
    {
        capy::continuation cont;

        resume_trampoline get_return_object()
        {
            return {
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept
        {
            return {};
        }
        std::suspend_never final_suspend() noexcept
        {
            return {};
        }
        void return_void() noexcept {}
        void unhandled_exception()
        {
            std::terminate();
        }
    };

    std::coroutine_handle<promise_type> h;
};

inline resume_trampoline
make_resume_trampoline(std::coroutine_handle<> target)
{
    // A nested resume, not a symmetric transfer that destroys this
    // frame inside await_suspend: that idiom is legal but miscompiled
    // by cl 19.44 and Apple Clang 16 (access violations on exactly
    // this path). The cost is one stack frame per executor hop, the
    // only path that reaches here.
    target.resume();
    co_return;
}

/** Dispatch a handle to an executor without lending it caller storage.

    Follows the executor's dispatch semantics: the resume may run
    inline or be queued, at the executor's discretion.

    Allocates one trampoline frame (capy's recycling frame pool);
    intended for the cold cross-executor completion path only. An
    allocation failure propagates out of the (noexcept-adjacent)
    completion path and terminates, matching the initiation paths'
    OOM policy. The caller may dispose its op before calling.

    @param ex The executor the resume must run on.
    @param h The coroutine to resume there.
*/
inline void
dispatch_resume(capy::executor_ref ex, std::coroutine_handle<> h)
{
    auto tramp               = make_resume_trampoline(h);
    tramp.h.promise().cont.h = tramp.h;
    ex.dispatch(tramp.h.promise().cont).resume();
}

/** Returns a handle for symmetric transfer on I/O completion.

    If the executor is io_context::executor_type, returns `c.h`
    directly (fast path). Otherwise the handle is dispatched through
    the executor via a trampoline frame and `noop_coroutine()` is
    returned.

    Callers in coroutine machinery should return the result
    for symmetric transfer. Callers at the scheduler pump
    level should call `.resume()` on the result.

    @p c is NOT retained past this call: completion ops embed their
    continuation in storage that is recycled, reused, or freed the
    moment the completion is consumed, so handing a deferring
    executor that storage lets a concurrent operation rewrite a
    queued node. The deferring branch therefore lends the executor a
    frame-owned continuation instead — see `dispatch_resume` — at the
    cost of one pooled frame on that cold path.

    @param ex The executor to dispatch through.
    @param c Carries the handle to resume; not retained.

    @return A handle for symmetric transfer or `std::noop_coroutine()`.
*/
/// Check whether @p ex is the io_context executor, directly or
/// type-erased through `capy::any_executor` (run_async callers and
/// tcp_server erase it that way). The held TYPE is enough:
/// completions only run on the run thread, where
/// `io_context::executor_type::dispatch` returns `c.h` inline —
/// exactly what the fast path reproduces.
inline bool
is_io_context_executor(capy::executor_ref ex) noexcept
{
    if (ex.target<io_context::executor_type>() != nullptr)
        return true;
    if (auto* any = ex.target<capy::any_executor>())
        return any->target_type() == typeid(io_context::executor_type);
    return false;
}

inline std::coroutine_handle<>
dispatch_coro(capy::executor_ref ex, capy::continuation& c)
{
    if (is_io_context_executor(ex))
        return c.h;
    dispatch_resume(ex, c.h);
    return std::noop_coroutine();
}

} // namespace boost::corosio::detail

#endif
