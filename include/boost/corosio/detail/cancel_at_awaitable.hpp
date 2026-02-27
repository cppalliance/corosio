//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_CANCEL_AT_AWAITABLE_HPP
#define BOOST_COROSIO_DETAIL_CANCEL_AT_AWAITABLE_HPP

#include <boost/corosio/detail/timeout_coro.hpp>
#include <boost/capy/ex/io_env.hpp>

#include <chrono>
#include <coroutine>
#include <new>
#include <optional>
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
   env and self-destroys via suspend_never. When Owning is
   false the caller-supplied timer must outlive both; when
   Owning is true the timer lives in std::optional and is
   constructed lazily in await_suspend. */

namespace boost::corosio::detail {

/** Awaitable adapter that cancels an inner operation after a deadline.

    Races the inner awaitable against a timer. A shared stop_source
    ties them together: whichever completes first cancels the other.
    Parent cancellation is forwarded via stop_callback.

    When @p Owning is `false` (default), the caller supplies a timer
    reference that must outlive the awaitable. When @p Owning is
    `true`, the timer is constructed internally in `await_suspend`
    from the execution context in `io_env`.

    @tparam A The inner IoAwaitable type (decayed).
    @tparam Timer The timer type (`timer` or `native_timer<B>`).
    @tparam Owning When `true`, the awaitable owns its timer.
*/
template<typename A, typename Timer, bool Owning = false>
struct cancel_at_awaitable
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
    using timer_storage =
        std::conditional_t<Owning, std::optional<Timer>, Timer*>;

    A inner_;
    timer_storage timer_;
    time_point deadline_;
    std::stop_source stop_src_;
    capy::io_env inner_env_;
    alignas(stop_cb_type) unsigned char cb_buf_[sizeof(stop_cb_type)];
    bool cb_active_ = false;

    /// Construct with a caller-supplied timer reference.
    cancel_at_awaitable(A&& inner, Timer& timer, time_point deadline)
        requires(!Owning)
        : inner_(std::move(inner))
        , timer_(&timer)
        , deadline_(deadline)
    {
    }

    /// Construct without a timer (created in `await_suspend`).
    cancel_at_awaitable(A&& inner, time_point deadline)
        requires Owning
        : inner_(std::move(inner))
        , deadline_(deadline)
    {
    }

    ~cancel_at_awaitable()
    {
        destroy_parent_cb();
    }

    // Only moved before await_suspend, when cb_active_ is false
    cancel_at_awaitable(cancel_at_awaitable&& o) noexcept(
        std::is_nothrow_move_constructible_v<A>)
        : inner_(std::move(o.inner_))
        , timer_(std::move(o.timer_))
        , deadline_(o.deadline_)
        , stop_src_(std::move(o.stop_src_))
    {
    }

    cancel_at_awaitable(cancel_at_awaitable const&)            = delete;
    cancel_at_awaitable& operator=(cancel_at_awaitable const&) = delete;
    cancel_at_awaitable& operator=(cancel_at_awaitable&&)      = delete;

    bool await_ready() const noexcept
    {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
    {
        if constexpr (Owning)
            timer_.emplace(env->executor.context());

        timer_->expires_at(deadline_);

        // Launch fire-and-forget timeout (starts suspended)
        auto timeout = make_timeout(*timer_, stop_src_);
        timeout.h_.promise().set_env_owned(
            {env->executor, stop_src_.get_token(), env->frame_allocator});
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
        // Cancel whichever is still pending (idempotent)
        stop_src_.request_stop();
        destroy_parent_cb();
        return inner_.await_resume();
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
