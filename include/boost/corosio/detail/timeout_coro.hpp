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

#include <boost/corosio/io_context.hpp>
#include <boost/capy/concept/context_awaitable.hpp>
#include <boost/capy/ex/context_awaitable_promise_base.hpp>
#include <boost/capy/ex/await_context.hpp>

#include <coroutine>
#include <memory_resource>
#include <stop_token>
#include <type_traits>
#include <utility>

/* Self-destroying coroutine that awaits a timer and signals a
   stop_source on expiry. Created suspended (initial_suspend =
   suspend_always); the caller sets an owned await_context copy then
   resumes, which runs synchronously until the timer wait suspends.
   At final_suspend, suspend_never destroys the frame — the
   timeout_coro destructor is intentionally a no-op since the
   handle is dangling after self-destruction. If the coroutine is
   still suspended at shutdown, the timer service drains it via
   completion_op::destroy().

   The promise base's default transform injects the owned
   await_context into each ContextAwaitable co_await expression.

   The detached frame can outlive the awaiting chain, so it never
   allocates from that chain's frame allocator: timeout_awaitable
   creates it under the process-wide recycling resource. */

namespace boost::corosio::detail {

/** Fire-and-forget coroutine backing `timeout()`.

    The coroutine awaits a timer and signals a stop_source if the
    timer fires without being cancelled. It self-destroys at
    final_suspend via suspend_never.

    @see make_timeout
*/
struct timeout_coro
{
    struct promise_type : capy::context_awaitable_promise_base<promise_type>
    {
        io_context::executor_type owned_ex_;
        capy::await_context env_storage_;

        /** Store an owned copy of the environment.

            The timeout coroutine can outlive the awaiting chain
            that created it, so it must own its env rather than
            pointing to external storage. The executor is owned by
            value: the chain's env holds only a ref to chain-owned
            executor storage, and the timer waiter's completion can
            run after that storage is gone. The ref handed to the
            waiter points at `owned_ex_` in this frame, which lives
            until final_suspend, after the waiter has completed.
        */
        void set_env_owned(
            io_context::executor_type ex,
            std::stop_token token,
            std::pmr::memory_resource* alloc)
        {
            owned_ex_    = ex;
            env_storage_ = {owned_ex_, std::move(token), alloc};
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

        // Only ContextAwaitables are awaited here; the base hands each
        // one the owned environment.
        template<class Awaitable>
        decltype(auto) transform_awaitable(Awaitable&& a)
        {
            static_assert(
                capy::ContextAwaitable<std::decay_t<Awaitable>>,
                "requires ContextAwaitable");
            return capy::context_awaitable_promise_base<
                promise_type>::transform_awaitable(std::forward<Awaitable>(a));
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

    @tparam Timer The timer type, typically `detail::timer`.

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
