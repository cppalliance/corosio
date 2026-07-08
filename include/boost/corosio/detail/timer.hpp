//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_TIMER_HPP
#define BOOST_COROSIO_DETAIL_TIMER_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/concept/executor.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <limits>
#include <stop_token>
#include <system_error>
#include <type_traits>

namespace boost::corosio::detail {

// Defined in timer_service.hpp, which includes this header. The
// implementation::wait() body needs waiter_node and timer_service to
// be complete, so it is declared here and defined there; intrusive_list
// only stores waiter_node pointers, so the forward declaration suffices
// for implementation's data layout.
class timer_service;
struct waiter_node;

/** An asynchronous timer for coroutine I/O.

    This class provides asynchronous timer operations that return
    awaitable types. The timer can be used to schedule operations
    to occur after a specified duration or at a specific time point.

    Multiple coroutines may wait concurrently on the same timer.
    When the timer expires, all waiters complete with success. When
    the timer is cancelled, all waiters complete with an error that
    compares equal to `capy::cond::canceled`.

    Each timer operation participates in the affine awaitable protocol,
    ensuring coroutines resume on the correct executor.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe.

    @par Semantics
    Timers are not backed by per-timer kernel objects. The io_context's
    timer service keeps a process-side min-heap of pending expirations;
    the nearest expiry drives the reactor's poll timeout, and expirations
    are processed in the run loop.
*/
class BOOST_COROSIO_DECL timer : public io_object
{
    struct wait_awaitable
    {
        timer& t_;
        std::error_code ec_;
        capy::continuation cont_;

        explicit wait_awaitable(timer& t) noexcept : t_(t) {}

        bool await_ready() const noexcept
        {
            return false;
        }

        // Cancellation surfaces through ec_: the stop_token path in
        // wait() completes the waiter with error::canceled written to
        // ec_, so there is no separate token to consult here.
        capy::io_result<> await_resume() const noexcept
        {
            return {ec_};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            cont_.h = h;
            auto& impl = t_.get();
            // Inline fast path: already expired and not in the heap
            if (impl.heap_index_.load(std::memory_order_relaxed) ==
                    implementation::npos &&
                (impl.expiry_ == (time_point::min)() ||
                 impl.expiry_ <= clock_type::now()))
            {
                ec_ = {};
                auto d = env->executor;
                d.post(cont_);
                return std::noop_coroutine();
            }
            return impl.wait(h, env->executor, env->stop_token, &ec_, &cont_);
        }
    };

public:
    /** Backend state and wait entry point for a timer.

        Holds per-timer state (expiry, heap position, waiter list) and
        the `wait` entry point used by the awaitable returned from
        @ref timer::wait. There is exactly one concrete timer backend,
        so `wait` is a plain member function rather than a virtual
        dispatch point.
    */
    struct implementation : io_object::implementation
    {
        /// Sentinel value indicating the timer is not in the heap.
        static constexpr std::size_t npos =
            (std::numeric_limits<std::size_t>::max)();

        // Only mutated by the owning thread (expires_at/expires_after)
        // before a wait is published; cross-thread consumers read the
        // heap entry's copied time_, never this field, so it needs no
        // atomicity.
        /// The absolute expiry time point.
        std::chrono::steady_clock::time_point expiry_{};

        // heap_index_ and might_have_pending_waits_ are cross-thread
        // hints, not authoritative state: the real state lives in the
        // heap and waiter list under timer_service::mutex_. Every
        // unlocked fast-out that reads them is either re-validated under
        // the mutex or safe under a stale value in both directions, and
        // any locked writer / locked reader pair is already ordered by
        // the mutex. All accesses therefore use memory_order_relaxed,
        // which keeps the lock-free fast paths fence-free while making
        // the concurrent reads well-defined.
        /// Index in the timer service's min-heap, or `npos`.
        std::atomic<std::size_t> heap_index_{npos};

        /// True if `wait()` has been called since last cancel.
        std::atomic<bool> might_have_pending_waits_{false};

        /// The timer service that owns this implementation.
        timer_service* svc_ = nullptr;

        /// Coroutines currently waiting on this timer's expiry.
        intrusive_list<waiter_node> waiters_;

        /// Free list linkage, reused when this impl is recycled.
        implementation* next_free_ = nullptr;

        /// Construct bound to the given timer service.
        explicit implementation(timer_service& svc) noexcept : svc_(&svc) {}

        /// Initiate an asynchronous wait for the timer to expire.
        // Exported at member level: dllexport on the enclosing timer
        // class does not extend to nested classes, and header-inline
        // callers (wait_awaitable::await_suspend) reference this
        // symbol from outside the corosio DLL.
        BOOST_COROSIO_DECL
        std::coroutine_handle<> wait(
            std::coroutine_handle<>,
            capy::executor_ref,
            std::stop_token,
            std::error_code*,
            capy::continuation*);
    };

    /// The clock type used for time operations.
    using clock_type = std::chrono::steady_clock;

    /// The time point type for absolute expiry times.
    using time_point = clock_type::time_point;

    /// The duration type for relative expiry times.
    using duration = clock_type::duration;

    /** Destructor.

        Cancels any pending operations and releases timer resources.
    */
    ~timer() override;

    /** Construct a timer from an execution context.

        @param ctx The execution context that will own this timer. It
            must be a corosio io_context; otherwise the constructor
            throws (a timer service is required).

        @throws std::logic_error if @p ctx is not an io_context.
    */
    explicit timer(capy::execution_context& ctx);

    /** Construct a timer with an initial absolute expiry time.

        @param ctx The execution context that will own this timer. It
            must be a corosio io_context; otherwise the constructor
            throws (a timer service is required).
        @param t The initial expiry time point.

        @throws std::logic_error if @p ctx is not an io_context.
    */
    timer(capy::execution_context& ctx, time_point t);

    /** Construct a timer with an initial relative expiry time.

        @param ctx The execution context that will own this timer. It
            must be a corosio io_context; otherwise the constructor
            throws (a timer service is required).
        @param d The initial expiry duration relative to now.

        @throws std::logic_error if @p ctx is not an io_context.
    */
    template<class Rep, class Period>
    timer(capy::execution_context& ctx, std::chrono::duration<Rep, Period> d)
        : timer(ctx)
    {
        expires_after(d);
    }

    /** Construct a timer from an executor.

        The timer is associated with the executor's context, which must
        be a corosio io_context.

        @param ex The executor whose context will own this timer.

        @throws std::logic_error if the executor's context is not an
            io_context.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, timer>) &&
        capy::Executor<Ex>
    explicit timer(Ex const& ex) : timer(ex.context())
    {
    }

    /** Construct a timer from an executor with an absolute expiry time.

        @param ex The executor whose context will own this timer.
        @param t The initial expiry time point.

        @throws std::logic_error if the executor's context is not an
            io_context.
    */
    template<class Ex>
        requires capy::Executor<Ex>
    timer(Ex const& ex, time_point t) : timer(ex.context(), t)
    {
    }

    /** Construct a timer from an executor with a relative expiry time.

        @param ex The executor whose context will own this timer.
        @param d The initial expiry duration relative to now.

        @throws std::logic_error if the executor's context is not an
            io_context.
    */
    template<class Ex, class Rep, class Period>
        requires capy::Executor<Ex>
    timer(Ex const& ex, std::chrono::duration<Rep, Period> d)
        : timer(ex.context(), d)
    {
    }

    /** Move constructor.

        Transfers ownership of the timer resources.

        @param other The timer to move from.

        @pre No awaitables returned by @p other's methods exist.
        @pre The execution context associated with @p other must
            outlive this timer.
    */
    timer(timer&& other) noexcept;

    /** Move assignment operator.

        Closes any existing timer and transfers ownership.

        @param other The timer to move from.

        @pre No awaitables returned by either `*this` or @p other's
            methods exist.
        @pre The execution context associated with @p other must
            outlive this timer.

        @return Reference to this timer.
    */
    timer& operator=(timer&& other) noexcept;

    timer(timer const&)            = delete;
    timer& operator=(timer const&) = delete;

    /** Cancel all pending asynchronous wait operations.

        All outstanding operations complete with an error code that
        compares equal to `capy::cond::canceled`.

        @return The number of operations that were cancelled.
    */
    std::size_t cancel()
    {
        if (!get().might_have_pending_waits_.load(std::memory_order_relaxed))
            return 0;
        return do_cancel();
    }

    /** Cancel one pending asynchronous wait operation.

        The oldest pending wait is cancelled (FIFO order). It
        completes with an error code that compares equal to
        `capy::cond::canceled`.

        @return The number of operations that were cancelled (0 or 1).
    */
    std::size_t cancel_one()
    {
        if (!get().might_have_pending_waits_.load(std::memory_order_relaxed))
            return 0;
        return do_cancel_one();
    }

    /** Return the timer's expiry time as an absolute time.

        @return The expiry time point. If no expiry has been set,
            returns a default-constructed time_point.
    */
    time_point expiry() const noexcept
    {
        return get().expiry_;
    }

    /** Set the timer's expiry time as an absolute time.

        Any pending asynchronous wait operations will be cancelled.

        @param t The expiry time to be used for the timer.

        @return The number of pending operations that were cancelled.
    */
    std::size_t expires_at(time_point t)
    {
        auto& impl   = get();
        impl.expiry_ = t;
        if (impl.heap_index_.load(std::memory_order_relaxed) ==
                implementation::npos &&
            !impl.might_have_pending_waits_.load(std::memory_order_relaxed))
            return 0;
        return do_update_expiry();
    }

    /** Set the timer's expiry time relative to now.

        Any pending asynchronous wait operations will be cancelled.

        @param d The expiry time relative to now.

        @return The number of pending operations that were cancelled.
    */
    std::size_t expires_after(duration d)
    {
        auto& impl = get();
        if (d <= duration::zero())
            impl.expiry_ = (time_point::min)();
        else
        {
            // Saturate rather than overflow: a clamped near-max duration
            // (e.g. delay(hours::max())) would wrap now() + d past the
            // clock's range and appear already elapsed.
            auto const now = clock_type::now();
            impl.expiry_ = ((time_point::max)() - now < d)
                ? (time_point::max)()
                : now + d;
        }
        if (impl.heap_index_.load(std::memory_order_relaxed) ==
                implementation::npos &&
            !impl.might_have_pending_waits_.load(std::memory_order_relaxed))
            return 0;
        return do_update_expiry();
    }

    /** Set the timer's expiry time relative to now.

        This is a convenience overload that accepts any duration type
        and converts it to the timer's native duration type. Any
        pending asynchronous wait operations will be cancelled.

        @param d The expiry time relative to now.

        @return The number of pending operations that were cancelled.
    */
    template<class Rep, class Period>
    std::size_t expires_after(std::chrono::duration<Rep, Period> d)
    {
        return expires_after(std::chrono::duration_cast<duration>(d));
    }

    /** Wait for the timer to expire.

        Multiple coroutines may wait on the same timer concurrently.
        When the timer expires, all waiters complete with success.

        The operation supports cancellation via `std::stop_token` through
        the affine awaitable protocol. If the associated stop token is
        triggered, only that waiter completes with an error that
        compares equal to `capy::cond::canceled`; other waiters are
        unaffected.

        This timer must outlive the returned awaitable.

        @return An awaitable that completes with `io_result<>`.
    */
    auto wait()
    {
        return wait_awaitable(*this);
    }

protected:
    explicit timer(handle h) noexcept : io_object(std::move(h)) {}

private:
    // Defined in src/corosio/src/timer.cpp, which includes both this
    // header and timer_service.hpp, so the timer_service_* free
    // functions are visible there.
    std::size_t do_cancel();
    std::size_t do_cancel_one();
    std::size_t do_update_expiry();

    /// Return the underlying implementation.
    implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio::detail

#endif
