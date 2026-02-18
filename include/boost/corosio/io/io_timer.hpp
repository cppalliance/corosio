//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_IO_TIMER_HPP
#define BOOST_COROSIO_IO_IO_TIMER_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/io_env.hpp>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <limits>
#include <stop_token>
#include <system_error>

namespace boost::corosio {

/** Abstract base for asynchronous timers.

    Provides the common timer interface: `wait`, `cancel`, and
    `expiry`. Concrete classes like @ref timer add the ability
    to set expiry times and cancel individual waiters.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Unsafe.

    @see timer, io_object
*/
class BOOST_COROSIO_DECL io_timer : public io_object
{
    struct wait_awaitable
    {
        io_timer& t_;
        std::stop_token token_;
        mutable std::error_code ec_;

        explicit wait_awaitable(io_timer& t) noexcept : t_(t) {}

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        capy::io_result<> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {capy::error::canceled};
            return {ec_};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_     = env->stop_token;
            auto& impl = t_.get();
            // Inline fast path: already expired and not in the heap
            if (impl.heap_index_ == implementation::npos &&
                (impl.expiry_ == (time_point::min)() ||
                 impl.expiry_ <= clock_type::now()))
            {
                ec_    = {};
                auto d = env->executor;
                d.post(h);
                return std::noop_coroutine();
            }
            return impl.wait(h, env->executor, std::move(token_), &ec_);
        }
    };

public:
    struct implementation : io_object::implementation
    {
        static constexpr std::size_t npos =
            (std::numeric_limits<std::size_t>::max)();

        std::chrono::steady_clock::time_point expiry_{};
        std::size_t heap_index_        = npos;
        bool might_have_pending_waits_ = false;

        virtual std::coroutine_handle<> wait(
            std::coroutine_handle<>,
            capy::executor_ref,
            std::stop_token,
            std::error_code*) = 0;
    };

    /// The clock type used for time operations.
    using clock_type = std::chrono::steady_clock;

    /// The time point type for absolute expiry times.
    using time_point = clock_type::time_point;

    /// The duration type for relative expiry times.
    using duration = clock_type::duration;

    /** Cancel all pending asynchronous wait operations.

        All outstanding operations complete with an error code that
        compares equal to `capy::cond::canceled`.

        @return The number of operations that were cancelled.
    */
    std::size_t cancel()
    {
        if (!get().might_have_pending_waits_)
            return 0;
        return do_cancel();
    }

    /** Return the timer's expiry time as an absolute time.

        @return The expiry time point. If no expiry has been set,
            returns a default-constructed time_point.
    */
    time_point expiry() const noexcept
    {
        return get().expiry_;
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
    /** Dispatch cancel to the concrete implementation.

        @return The number of operations that were cancelled.
    */
    virtual std::size_t do_cancel() = 0;

    explicit io_timer(handle h) noexcept : io_object(std::move(h)) {}

    /// Move construct.
    io_timer(io_timer&& other) noexcept : io_object(std::move(other)) {}

    /// Move assign.
    io_timer& operator=(io_timer&& other) noexcept
    {
        if (this != &other)
            h_ = std::move(other.h_);
        return *this;
    }

    io_timer(io_timer const&)            = delete;
    io_timer& operator=(io_timer const&) = delete;

    /// Return the underlying implementation.
    implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif
