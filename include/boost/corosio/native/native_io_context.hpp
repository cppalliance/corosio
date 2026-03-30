//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_IO_CONTEXT_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_IO_CONTEXT_HPP

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/backend.hpp>

#ifndef BOOST_COROSIO_MRDOCS
#if BOOST_COROSIO_HAS_EPOLL
#include <boost/corosio/native/detail/epoll/epoll_scheduler.hpp>
#endif

#if BOOST_COROSIO_HAS_SELECT
#include <boost/corosio/native/detail/select/select_scheduler.hpp>
#endif

#if BOOST_COROSIO_HAS_KQUEUE
#include <boost/corosio/native/detail/kqueue/kqueue_scheduler.hpp>
#endif

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#endif
#endif // !BOOST_COROSIO_MRDOCS

namespace boost::corosio {

/** An I/O context with devirtualized event loop methods.

    This class template inherits from @ref io_context and shadows
    all public methods with versions that call the concrete
    scheduler directly, bypassing virtual dispatch. No new state
    is added.

    A `native_io_context` IS-A `io_context` and can be passed
    anywhere an `io_context&` is accepted, in which case virtual
    dispatch is used transparently.

    @tparam Backend A backend tag value (e.g., `epoll`,
        `iocp`) whose type provides `scheduler_type`.

    @par Thread Safety
    Same as the underlying context type.

    @par Example
    @code
    #include <boost/corosio/native/native_io_context.hpp>

    native_io_context<epoll> ctx;
    ctx.poll();  // devirtualized call
    @endcode

    @see io_context, epoll_t, iocp_t
*/
template<auto Backend>
class native_io_context : public io_context
{
    using backend_type   = decltype(Backend);
    using scheduler_type = typename backend_type::scheduler_type;

    scheduler_type& sched() noexcept
    {
        return *static_cast<scheduler_type*>(this->sched_);
    }

public:
    /** Construct with default concurrency. */
    native_io_context() : io_context(Backend) {}

    /** Construct with a concurrency hint.

        @param concurrency_hint Hint for the number of threads that
            will call `run()`.
    */
    explicit native_io_context(unsigned concurrency_hint)
        : io_context(Backend, concurrency_hint)
    {
    }

    /** Construct with runtime tuning options.

        @param opts Runtime options controlling scheduler and
            service behavior.
        @param concurrency_hint Hint for the number of threads that
            will call `run()`.
    */
    explicit native_io_context(
        io_context_options const& opts,
        unsigned concurrency_hint = std::thread::hardware_concurrency())
        : io_context(Backend, opts, concurrency_hint)
    {
    }

    // Non-copyable, non-movable
    native_io_context(native_io_context const&)            = delete;
    native_io_context& operator=(native_io_context const&) = delete;

    /// Signal the context to stop processing.
    void stop()
    {
        sched().stop();
    }

    /// Return whether the context has been stopped.
    bool stopped() const noexcept
    {
        return const_cast<native_io_context*>(this)->sched().stopped();
    }

    /// Restart the context after being stopped.
    void restart()
    {
        sched().restart();
    }

    /** Process all pending work items.

        @return The number of handlers executed.
    */
    std::size_t run()
    {
        return sched().run();
    }

    /** Process at most one pending work item.

        @return The number of handlers executed (0 or 1).
    */
    std::size_t run_one()
    {
        return sched().run_one();
    }

    /** Process work items for the specified duration.

        @param rel_time The duration for which to process work.

        @return The number of handlers executed.
    */
    template<class Rep, class Period>
    std::size_t run_for(std::chrono::duration<Rep, Period> const& rel_time)
    {
        return run_until(std::chrono::steady_clock::now() + rel_time);
    }

    /** Process work items until the specified time.

        @param abs_time The time point until which to process work.

        @return The number of handlers executed.
    */
    template<class Clock, class Duration>
    std::size_t
    run_until(std::chrono::time_point<Clock, Duration> const& abs_time)
    {
        std::size_t n = 0;
        while (run_one_until(abs_time))
            if (n != (std::numeric_limits<std::size_t>::max)())
                ++n;
        return n;
    }

    /** Process at most one work item for the specified duration.

        @param rel_time The duration for which the call may block.

        @return The number of handlers executed (0 or 1).
    */
    template<class Rep, class Period>
    std::size_t run_one_for(std::chrono::duration<Rep, Period> const& rel_time)
    {
        return run_one_until(std::chrono::steady_clock::now() + rel_time);
    }

    /** Process at most one work item until the specified time.

        @param abs_time The time point until which the call may block.

        @return The number of handlers executed (0 or 1).
    */
    template<class Clock, class Duration>
    std::size_t
    run_one_until(std::chrono::time_point<Clock, Duration> const& abs_time)
    {
        typename Clock::time_point now = Clock::now();
        while (now < abs_time)
        {
            auto rel_time = abs_time - now;
            if (rel_time > std::chrono::seconds(1))
                rel_time = std::chrono::seconds(1);

            std::size_t s = sched().wait_one(
                static_cast<long>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        rel_time)
                        .count()));

            if (s || stopped())
                return s;

            now = Clock::now();
        }
        return 0;
    }

    /** Process all ready work items without blocking.

        @return The number of handlers executed.
    */
    std::size_t poll()
    {
        return sched().poll();
    }

    /** Process at most one ready work item without blocking.

        @return The number of handlers executed (0 or 1).
    */
    std::size_t poll_one()
    {
        return sched().poll_one();
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_NATIVE_NATIVE_IO_CONTEXT_HPP
