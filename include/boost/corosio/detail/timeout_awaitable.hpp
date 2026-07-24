//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_TIMEOUT_AWAITABLE_HPP
#define BOOST_COROSIO_DETAIL_TIMEOUT_AWAITABLE_HPP

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/detail/timeout_coro.hpp>
#include <boost/corosio/detail/timer.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/io_result.hpp>

#include <chrono>
#include <coroutine>
#include <new>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>

/* Races an inner IoAwaitable against a timer via a shared
   stop_source. await_suspend arms the timer by launching a
   fire-and-forget timeout_coro, then starts the inner op with
   an interposed stop_token. Whichever completes first signals
   the stop_source, cancelling the other.

   Parent cancellation is forwarded through a stop_callback
   stored in a placement-new buffer (stop_callback is not
   movable, but the awaitable must be movable for
   transform_awaiter). The buffer is inert during moves
   (before await_suspend) and constructed in-place once the
   awaitable is pinned on the coroutine frame.

   The timeout_coro can outlive this awaitable — it owns its
   env and self-destroys via suspend_never. The timer lives in
   std::optional and is constructed lazily in await_suspend,
   once the awaiting coroutine's executor context is known. */

namespace boost::corosio::detail {

// Local stand-in for capy::detail's io_result trait: corosio must not
// reach into capy::detail, but the result-mapping switch in
// await_resume needs to distinguish io_result from other return types.
template<typename T>
struct is_io_result : std::false_type
{
};

template<typename... Ts>
struct is_io_result<capy::io_result<Ts...>> : std::true_type
{
};

template<typename T>
inline constexpr bool is_io_result_v = is_io_result<T>::value;

/** Awaitable adapter that cancels an inner operation after a deadline.

    Races the inner awaitable against a timer. A shared stop_source
    ties them together: whichever completes first cancels the other.
    Parent cancellation is forwarded via stop_callback.

    The timer is constructed internally in `await_suspend` from the
    execution context in `io_env`.

    @tparam A The inner IoAwaitable type (decayed).
*/
template<typename A>
struct timeout_awaitable
{
    struct stop_forwarder
    {
        std::stop_source* src_;
        void operator()() const noexcept
        {
            src_->request_stop();
        }
    };

    using time_point   = std::chrono::steady_clock::time_point;
    using stop_cb_type = std::stop_callback<stop_forwarder>;

    A inner_;
    std::optional<timer> timer_;
    time_point deadline_;
    std::chrono::nanoseconds dur_{};
    bool has_deadline_ = true;
    std::stop_source stop_src_;
    std::stop_token parent_token_;
    capy::io_env inner_env_;
    alignas(stop_cb_type) unsigned char cb_buf_[sizeof(stop_cb_type)];
    bool cb_active_ = false;

    /// Construct without a timer, deadline given as an absolute time.
    timeout_awaitable(A&& inner, time_point deadline)
        : inner_(std::move(inner))
        , deadline_(deadline)
    {
    }

    /// Construct without a timer, deadline measured from suspension.
    timeout_awaitable(A&& inner, std::chrono::nanoseconds dur)
        : inner_(std::move(inner))
        , dur_(dur)
        , has_deadline_(false)
    {
    }

    ~timeout_awaitable()
    {
        destroy_parent_cb();
    }

    // Only moved before await_suspend, when cb_active_ is false
    timeout_awaitable(timeout_awaitable&& o) noexcept(
        std::is_nothrow_move_constructible_v<A>)
        : inner_(std::move(o.inner_))
        , timer_(std::move(o.timer_))
        , deadline_(o.deadline_)
        , dur_(o.dur_)
        , has_deadline_(o.has_deadline_)
        , stop_src_(std::move(o.stop_src_))
    {
    }

    timeout_awaitable(timeout_awaitable const&)            = delete;
    timeout_awaitable& operator=(timeout_awaitable const&) = delete;
    timeout_awaitable& operator=(timeout_awaitable&&)      = delete;

    // Forwarding here is load-bearing, not an optimization: awaitables
    // may perform setup in await_ready (type-erased stream wrappers
    // construct their cached inner op there), so the full awaiter
    // protocol must reach inner_ before await_suspend is driven. An
    // already-ready inner op also skips arming the timer entirely.
    bool await_ready()
    {
        return inner_.await_ready();
    }

    auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
    {
        parent_token_ = env->stop_token;

        // The deadline timer is built here from the awaiting
        // coroutine's executor context, the first point at which it
        // is known. await_suspend is driven through a noexcept
        // wrapper, so a failure cannot be surfaced as a catchable
        // exception. An executor whose context is not an io_context
        // cannot supply a timer service; silently running the
        // operation with no deadline would be a worse failure than
        // aborting, so translate the service-lookup error into a
        // clear precondition diagnostic. This terminates by design
        // (a usage error) rather than dropping the requested timeout.
        // The detached timeout coroutine must own its executor by
        // value (see timeout_coro::set_env_owned); io_env carries
        // only a non-owning executor_ref. Recover the concrete
        // executor from the context rather than the executor_ref:
        // wrapped executors (a strand over the io_context) satisfy
        // the documented precondition but do not expose the io
        // executor as their target. The timer construction below
        // validates the context is an io_context, and the detached
        // coroutine shares only the thread-safe stop_source with
        // the caller, so resuming it on the raw io executor instead
        // of the caller's wrapper is safe.
        try
        {
            timer_.emplace(env->executor.context());
        }
        catch (std::logic_error const&)
        {
            throw_logic_error(
                "timeout requires an io_context-backed executor");
        }
        auto ex = static_cast<io_context&>(
            env->executor.context()).get_executor();

        if (has_deadline_)
            timer_->expires_at(deadline_);
        else
            timer_->expires_after(dur_);

        // Launch fire-and-forget timeout (starts suspended)
        auto timeout = make_timeout(*timer_, stop_src_);
        timeout.h_.promise().set_env_owned(
            ex, stop_src_.get_token(), env->frame_allocator);
        // Runs synchronously until timer.wait() suspends
        timeout.h_.resume();
        // timeout goes out of scope; destructor is a no-op,
        // the coroutine self-destroys via suspend_never

        // Forward parent cancellation
        new (cb_buf_) stop_cb_type(env->stop_token, stop_forwarder{&stop_src_});
        cb_active_ = true;

        // Start the inner op with our interposed stop_token
        inner_env_ = {
            env->executor, stop_src_.get_token(), env->frame_allocator};
        return inner_.await_suspend(h, &inner_env_);
    }

    decltype(auto) await_resume()
    {
        // Read before request_stop: afterwards stop_requested()
        // can no longer distinguish who fired first. This must also
        // happen before inner_.await_resume() rather than after: when
        // the inner awaitable is itself a timeout_awaitable (nested
        // timeout()), our own request_stop() below is visible through
        // its parent_token_ (aliasing our stop_src_), and would
        // otherwise make its read of "parent" look like a
        // cancellation that never happened.
        bool const parent = parent_token_.stop_requested();
        bool const fired  = stop_src_.stop_requested();

        // If inner_.await_resume() throws below, request_stop() is
        // skipped; the still-armed timeout coroutine is then drained
        // by timer_'s destructor rather than by us.
        auto r = inner_.await_resume();

        // Cancel whichever is still pending (idempotent)
        stop_src_.request_stop();
        destroy_parent_cb();

        // Deadline won: stop_src_ is assumed to be the only
        // cancellation source, whose only writers are the timer
        // coroutine and the parent forwarder, so fired && !parent
        // identifies a timeout. A third-party cancellation of the
        // inner op (e.g. a socket cancel issued from elsewhere)
        // landing in the same window as the deadline firing is
        // reported as a timeout.
        if (fired && !parent &&
            r.ec == capy::cond::canceled)
        {
            std::remove_cvref_t<decltype(r)> t{};
            t.ec = make_error_code(capy::error::timeout);
            return t;
        }
        return r;
    }

    void destroy_parent_cb() noexcept
    {
        if (cb_active_)
        {
            std::launder(reinterpret_cast<stop_cb_type*>(cb_buf_))
                ->~stop_cb_type();
            cb_active_ = false;
        }
    }
};

} // namespace boost::corosio::detail

#endif
