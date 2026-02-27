//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_TIMEOUT_CORO_HPP
#define BOOST_COROSIO_DETAIL_TIMEOUT_CORO_HPP

#include <boost/capy/concept/io_awaitable.hpp>
#include <boost/capy/ex/frame_allocator.hpp>
#include <boost/capy/ex/io_awaitable_promise_base.hpp>
#include <boost/capy/ex/io_env.hpp>

#include <coroutine>
#include <stop_token>
#include <type_traits>
#include <utility>

/* Self-destroying coroutine that awaits a timer and signals a
   stop_source on expiry. Created suspended (initial_suspend =
   suspend_always); the caller sets an owned io_env copy then
   resumes, which runs synchronously until the timer wait suspends.
   At final_suspend, suspend_never destroys the frame — the
   timeout_coro destructor is intentionally a no-op since the
   handle is dangling after self-destruction. If the coroutine is
   still suspended at shutdown, the timer service drains it via
   completion_op::destroy().

   The promise reuses task<>'s transform_awaiter pattern (including
   the MSVC symmetric-transfer workaround) to inject io_env into
   IoAwaitable co_await expressions. */

namespace boost::corosio::detail {

/** Fire-and-forget coroutine for the timeout side of cancel_at.

    The coroutine awaits a timer and signals a stop_source if the
    timer fires without being cancelled. It self-destroys at
    final_suspend via suspend_never.

    @see make_timeout
*/
struct timeout_coro
{
    struct promise_type : capy::io_awaitable_promise_base<promise_type>
    {
        capy::io_env env_storage_;

        /** Store an owned copy of the environment.

            The timeout coroutine can outlive the cancel_at_awaitable
            that created it, so it must own its env rather than
            pointing to external storage.
        */
        void set_env_owned(capy::io_env env)
        {
            env_storage_ = std::move(env);
            set_environment(&env_storage_);
        }

        timeout_coro get_return_object() noexcept
        {
            return timeout_coro{
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
        void unhandled_exception() noexcept {}

        template<class Awaitable>
        struct transform_awaiter
        {
            std::decay_t<Awaitable> a_;
            promise_type* p_;

            bool await_ready() noexcept
            {
                return a_.await_ready();
            }

            decltype(auto) await_resume()
            {
                capy::set_current_frame_allocator(
                    p_->environment()->frame_allocator);
                return a_.await_resume();
            }

            template<class Promise>
            auto await_suspend(std::coroutine_handle<Promise> h) noexcept
            {
#ifdef _MSC_VER
                using R = decltype(a_.await_suspend(h, p_->environment()));
                if constexpr (std::is_same_v<R, std::coroutine_handle<>>)
                    a_.await_suspend(h, p_->environment()).resume();
                else
                    return a_.await_suspend(h, p_->environment());
#else
                return a_.await_suspend(h, p_->environment());
#endif
            }
        };

        template<class Awaitable>
        auto transform_awaitable(Awaitable&& a)
        {
            using A = std::decay_t<Awaitable>;
            if constexpr (capy::IoAwaitable<A>)
            {
                return transform_awaiter<Awaitable>{
                    std::forward<Awaitable>(a), this};
            }
            else
            {
                static_assert(sizeof(A) == 0, "requires IoAwaitable");
            }
        }
    };

    std::coroutine_handle<promise_type> h_;

    timeout_coro() noexcept : h_(nullptr) {}

    explicit timeout_coro(std::coroutine_handle<promise_type> h) noexcept
        : h_(h)
    {
    }

    // Self-destroying via suspend_never at final_suspend
    ~timeout_coro() = default;

    timeout_coro(timeout_coro const&)            = delete;
    timeout_coro& operator=(timeout_coro const&) = delete;

    timeout_coro(timeout_coro&& o) noexcept : h_(o.h_)
    {
        o.h_ = nullptr;
    }

    timeout_coro& operator=(timeout_coro&& o) noexcept
    {
        h_   = o.h_;
        o.h_ = nullptr;
        return *this;
    }
};

/** Create a fire-and-forget timeout coroutine.

    Wait on the timer. If it fires without cancellation, signal
    the stop source to cancel the paired inner operation.

    @tparam Timer Timer type (`timer` or `native_timer<B>`).

    @param t The timer to wait on (must have expiry set).
    @param src Stop source to signal on timeout.
*/
template<typename Timer>
timeout_coro
make_timeout(Timer& t, std::stop_source src)
{
    auto [ec] = co_await t.wait();
    if (!ec)
        src.request_stop();
}

} // namespace boost::corosio::detail

#endif
