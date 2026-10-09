//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_OBJECT_HANDLE_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_OBJECT_HANDLE_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/detail/object_pool.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/detail/win_handle_service.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_handle.hpp>
#include <boost/corosio/native/detail/iocp/win_validate_handle.hpp>
#include <boost/capy/error.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>

/* win_object_handle on the Windows thread pool.

   One PTP_WAIT per state, created at the first wait() and re-armed per
   wait. The claim word (phase in the low two bits, a generation above)
   decides which of the pool callback and a cancellation delivers the
   one op: whoever moves it from armed to claimed posts the op.

   Cancellation never cancels a queued callback. By the time a callback
   is queued the kernel has already satisfied the wait -- consumed an
   auto-reset event's signal, a semaphore unit -- and the callback is
   the only witness to that. So every rundown (cancel, stop token,
   close, release, assign, destroy, shutdown) drains instead:
   SetThreadpoolWait(nullptr) stops an unsatisfied wait, then
   WaitForThreadpoolWaitCallbacks(FALSE) lets a queued callback finish
   and claim, and only then does the rundown try to claim for
   cancellation.

   arm_mutex_ orders arming against the disarm step, and disarmed_gen_
   records which generation a rundown disarmed, so a rundown that
   disarmed before wait() armed is never followed by that arm. The
   callback never takes the mutex, which keeps the drain deadlock-free.
*/

namespace boost::corosio::detail {

/** Object handle state, embedded in the implementation named by `io`.

    The thread-pool wait survives recycling and is closed with the
    implementation's storage.
*/
class win_object_handle_state
{
public:
    win_object_handle_state(
        win_scheduler& sched, io_object::implementation& io) noexcept
        : sched_(sched)
        , io_(io)
    {
        op_.self = this;
    }

    ~win_object_handle_state()
    {
        if (tp_wait_)
        {
            ::SetThreadpoolWait(tp_wait_, nullptr, nullptr);
            ::WaitForThreadpoolWaitCallbacks(tp_wait_, FALSE);
            ::CloseThreadpoolWait(tp_wait_);
        }
    }

    win_object_handle_state(win_object_handle_state const&) = delete;
    win_object_handle_state& operator=(win_object_handle_state const&) = delete;

    HANDLE native_handle() const noexcept
    {
        return handle_;
    }

    bool is_open() const noexcept
    {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    std::coroutine_handle<> wait(
        capy::continuation& cont,
        capy::executor_ref ex,
        std::stop_token token,
        std::error_code* ec)
    {
        // Reserved as claimed under the current generation until armed:
        // a concurrent wait() sees it busy, nothing else claims it, and
        // a rundown in the meantime records a generation this wait never
        // uses.
        std::uint64_t cur = state_.load(std::memory_order_acquire);
        if ((cur & phase_mask) != phase_idle ||
            !state_.compare_exchange_strong(
                cur, cur | phase_claimed, std::memory_order_acq_rel))
        {
            // Complete immediately: the returned handle is resumed by
            // symmetric transfer on the caller's own executor.
            if (ec)
                *ec = std::make_error_code(std::errc::operation_in_progress);
            return cont.h;
        }

        std::uint64_t const gen   = (cur >> 2) + 1;
        std::uint64_t const armed = (gen << 2) | phase_armed;

        op_.reset();
        op_.object_ref_ = detail::object_ref(&io_);
        // The op may complete and the object be destroyed on another
        // thread before wait() returns; this pin outlives both.
        auto const pin = op_.object_ref_;
        op_.cont       = &cont;
        op_.ex         = ex;
        op_.ec_out     = ec;
        op_.bytes_out  = nullptr;
        sched_.work_started();

        if (handle_ == INVALID_HANDLE_VALUE)
        {
            state_.store((gen << 2) | phase_claimed, std::memory_order_release);
            sched_.on_completion(&op_, ERROR_INVALID_HANDLE, 0);
            return std::noop_coroutine();
        }

        if (!tp_wait_)
        {
            tp_wait_ = ::CreateThreadpoolWait(&on_signaled, this, nullptr);
            if (!tp_wait_)
            {
                state_.store(
                    (gen << 2) | phase_claimed, std::memory_order_release);
                sched_.on_completion(&op_, ::GetLastError(), 0);
                return std::noop_coroutine();
            }
        }

        // The stop callback is engaged before the wait is published, so
        // a completion can never find it mid-construction. A stop that
        // runs before publication finds nothing to claim; the check
        // below claims for it.
        op_.start(token);
        state_.store(armed, std::memory_order_release);
        {
            // A rundown that disarmed this generation first has claimed,
            // or will claim, the op; arming now would let a later signal
            // be consumed with nobody left to report it.
            std::lock_guard<win_mutex> lock(arm_mutex_);
            if (state_.load(std::memory_order_acquire) == armed &&
                disarmed_gen_ != gen)
            {
                if (!op_.cancelled.load(std::memory_order_acquire))
                    ::SetThreadpoolWait(tp_wait_, handle_, nullptr);
                else if (claim(armed))
                    sched_.on_completion(&op_, ERROR_OPERATION_ABORTED, 0);
            }
        }
        return std::noop_coroutine();
    }

    void cancel() noexcept
    {
        rundown();
    }

    void close_handle() noexcept
    {
        rundown();
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    native_handle_type release() noexcept
    {
        rundown();
        HANDLE h = handle_;
        handle_  = INVALID_HANDLE_VALUE;
        return reinterpret_cast<native_handle_type>(h);
    }

    /// Return true if no wait is armed or being delivered.
    bool is_idle() const noexcept
    {
        return (state_.load(std::memory_order_acquire) & phase_mask) ==
            phase_idle;
    }

    std::error_code assign(native_handle_type nh) noexcept
    {
        HANDLE h = reinterpret_cast<HANDLE>(nh);
        if (auto ec = validate_object_handle(h))
            return ec;
        handle_ = h;
        return {};
    }

private:
    static constexpr std::uint64_t phase_idle    = 0;
    static constexpr std::uint64_t phase_armed   = 1;
    static constexpr std::uint64_t phase_claimed = 2;
    static constexpr std::uint64_t phase_mask    = 3;

    struct wait_op : overlapped_op
    {
        win_object_handle_state* self = nullptr;

        wait_op() noexcept : overlapped_op(&do_complete)
        {
            cancel_func_ = &do_cancel_impl;
        }

        static void do_cancel_impl(overlapped_op* base) noexcept
        {
            static_cast<wait_op*>(base)->self->rundown();
        }

        static void do_complete(
            void* owner,
            scheduler_op* base,
            std::uint32_t /*bytes*/,
            std::uint32_t /*error*/)
        {
            auto* op   = static_cast<wait_op*>(base);
            auto* self = op->self;
            auto prevent_premature_destruction = std::move(op->object_ref_);

            if (!owner)
            {
                self->set_idle();
                op->cleanup_only();
                return;
            }

            op->stop_cb.reset();
            // A claimed signal is success even if a cancel request raced
            // it and set the cancelled flag: completion wins.
            if (op->ec_out)
            {
                if (op->dwError == 0)
                    *op->ec_out = {};
                else if (op->dwError == ERROR_OPERATION_ABORTED)
                    *op->ec_out = capy::error::canceled;
                else
                    *op->ec_out =
                        iocp_make_err(op->dwError, /*accept_path=*/false);
            }

            capy::continuation* cont = op->cont;
            capy::executor_ref ex    = op->ex;

            // Last touch of op_: from here a new wait() may reset it.
            self->set_idle();
            dispatch_coro(ex, *cont).resume();
        }
    };

    // Back to idle, keeping the generation.
    void set_idle() noexcept
    {
        state_.store(
            state_.load(std::memory_order_relaxed) & ~phase_mask,
            std::memory_order_release);
    }

    static void CALLBACK on_signaled(
        PTP_CALLBACK_INSTANCE, void* ctx, PTP_WAIT, TP_WAIT_RESULT) noexcept
    {
        auto* self = static_cast<win_object_handle_state*>(ctx);
        std::uint64_t cur = self->state_.load(std::memory_order_acquire);
        if ((cur & phase_mask) == phase_armed && self->claim(cur))
            self->sched_.on_completion(&self->op_, 0, 0);
    }

    bool claim(std::uint64_t armed) noexcept
    {
        return state_.compare_exchange_strong(
            armed, (armed & ~phase_mask) | phase_claimed,
            std::memory_order_acq_rel);
    }

    // Stop an unsatisfied wait, let a queued callback finish, then claim
    // for cancellation only if nothing else did. Never called from the
    // pool callback.
    void rundown() noexcept
    {
        if (!tp_wait_)
            return;
        {
            std::lock_guard<win_mutex> lock(arm_mutex_);
            ::SetThreadpoolWait(tp_wait_, nullptr, nullptr);
            disarmed_gen_ = state_.load(std::memory_order_acquire) >> 2;
        }
        ::WaitForThreadpoolWaitCallbacks(tp_wait_, FALSE);

        std::uint64_t cur = state_.load(std::memory_order_acquire);
        if ((cur & phase_mask) == phase_armed && claim(cur))
        {
            op_.request_cancel();
            sched_.on_completion(&op_, ERROR_OPERATION_ABORTED, 0);
        }
    }

    win_scheduler& sched_;
    io_object::implementation& io_;
    HANDLE handle_    = INVALID_HANDLE_VALUE;
    PTP_WAIT tp_wait_ = nullptr;
    std::atomic<std::uint64_t> state_{phase_idle};
    win_mutex arm_mutex_;
    std::uint64_t disarmed_gen_ = 0; // guarded by arm_mutex_
    wait_op op_;
};

class win_object_handle_service;

/** IOCP implementation of @ref win_object_handle. */
class win_object_handle_impl final
    : public win_object_handle::implementation
    , public intrusive_list<win_object_handle_impl>::node
{
    win_object_handle_service& svc_;
    win_object_handle_state internal_;

public:
    win_object_handle_impl(
        win_object_handle_service& svc, win_scheduler& sched) noexcept
        : svc_(svc)
        , internal_(sched, *this)
    {
    }

    /// Recycle into the owning service's pool. Defined out-of-line
    /// after the service is complete.
    void retire() noexcept override;

    /** Assert the closed state a recycled handle starts from.

        @pre refs_ == 0, handle closed, no wait in flight.
    */
    void reuse() noexcept
    {
        BOOST_COROSIO_ASSERT(!internal_.is_open());
        BOOST_COROSIO_ASSERT(internal_.is_idle());
    }

    win_object_handle_state* get_internal() noexcept
    {
        return &internal_;
    }

    std::coroutine_handle<> wait(
        capy::continuation& cont,
        capy::executor_ref ex,
        std::stop_token token,
        std::error_code* ec) override
    {
        return internal_.wait(cont, ex, std::move(token), ec);
    }

    native_handle_type native_handle() const noexcept override
    {
        return reinterpret_cast<native_handle_type>(internal_.native_handle());
    }

    native_handle_type release_handle() noexcept override
    {
        return internal_.release();
    }

    void cancel() noexcept override
    {
        internal_.cancel();
    }
};

/** IOCP service that owns the @ref win_object_handle implementations. */
class BOOST_COROSIO_DECL win_object_handle_service final
    : public object_handle_service
{
    friend class win_object_handle_impl;

public:
    explicit win_object_handle_service(capy::execution_context& ctx)
        : sched_(ctx.use_service<win_scheduler>())
    {
    }

    io_object::implementation* construct() override
    {
        return pool_.acquire(*this, sched_);
    }

    void destroy(io_object::implementation* p) override
    {
        if (!p)
            return;
        auto* impl = static_cast<win_object_handle_impl*>(p);
        impl->get_internal()->close_handle();
        release(impl);
    }

    void close(io_object::handle& h) override
    {
        static_cast<win_object_handle_impl&>(*h.get())
            .get_internal()
            ->close_handle();
    }

    void shutdown() override
    {
        pool_.shutdown([](win_object_handle_impl* impl) {
            impl->get_internal()->close_handle();
        });
    }

    std::error_code assign_object_handle(
        win_object_handle::implementation& impl,
        native_handle_type h) override
    {
        // The thread-pool callback completes from a foreign thread.
        if (sched_.scheduler_locking_disabled())
            return std::make_error_code(std::errc::operation_not_supported);
        return static_cast<win_object_handle_impl&>(impl)
            .get_internal()
            ->assign(h);
    }

private:
    win_scheduler& sched_;
    BOOST_COROSIO_MSVC_WARNING_PUSH
    BOOST_COROSIO_MSVC_WARNING_DISABLE(4251) // detail:: members, dll-interface
    object_pool<win_object_handle_impl> pool_;
    BOOST_COROSIO_MSVC_WARNING_POP
};

inline void
win_object_handle_impl::retire() noexcept
{
    svc_.pool_.recycle(this);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif
