//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_SPECULATIVE_STATE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_SPECULATIVE_STATE_HPP

#include <atomic>

namespace boost::corosio::detail {

/** Per-socket per-op-type speculative-attempt hint.

    Tracks whether a speculative non-blocking syscall is worth trying
    for read and write paths. The flag is set false when speculation
    discovers an exhausted buffer (EAGAIN) and restored when the async
    completion path observes a kernel readiness signal.

    Atomics are relaxed because the flag is a hint, not an invariant:
    a stale read causes at most one wasted or skipped speculation, never
    a correctness failure.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Safe.
*/
class speculative_state
{
    std::atomic< bool > try_read_ { true };
    std::atomic< bool > try_write_{ true };

    // Failure-streak counter for the read path. Increments on every
    // speculative-read EAGAIN; resets to 0 whenever a speculative read
    // succeeds. When it reaches max_read_failures the socket gives up
    // on speculative reads permanently — perma_off_read_ latches and
    // may_speculate_read() returns false regardless of any subsequent
    // async-read re-arm signal.
    //
    // Distinguishes "transient EAGAIN under heavy success" (e.g.
    // socket_throughput streaming: 1 EAGAIN per ~100 successes ->
    // streak resets, perma-off never triggers) from "structural EAGAIN
    // pattern" (e.g. fan_out:nested/16: every speculation EAGAINs ->
    // streak hits max_read_failures and we stop wasting syscalls).
    static constexpr int max_read_failures = 4;
    std::atomic< int >  read_eagain_streak_ { 0 };
    std::atomic< bool > perma_off_read_     { false };

public:
    /// Return true when speculative read is currently worth trying.
    bool may_speculate_read() const noexcept
    {
        return try_read_.load( std::memory_order_relaxed )
            && !perma_off_read_.load( std::memory_order_relaxed );
    }

    /// Return true when speculative write is currently worth trying.
    bool may_speculate_write() const noexcept
    {
        return try_write_.load( std::memory_order_relaxed );
    }

    /// Disable speculative reads (kernel buffer is empty).
    /// Tracks the failure streak; permanently disables speculation
    /// for this socket once the streak hits max_read_failures.
    void on_read_exhausted() noexcept
    {
        try_read_.store( false, std::memory_order_relaxed );
        int s = read_eagain_streak_.load( std::memory_order_relaxed );
        if ( s < max_read_failures )
        {
            ++s;
            read_eagain_streak_.store( s, std::memory_order_relaxed );
            if ( s >= max_read_failures )
                perma_off_read_.store( true, std::memory_order_relaxed );
        }
    }

    /// Reset the failure streak on a successful speculative read. The
    /// successful syscall is proof that the workload pattern *does*
    /// hit speculation often enough to be worth the occasional EAGAIN.
    void on_read_success() noexcept
    {
        if ( read_eagain_streak_.load( std::memory_order_relaxed ) != 0 )
            read_eagain_streak_.store( 0, std::memory_order_relaxed );
    }

    /// Disable speculative writes (kernel buffer is full).
    void on_write_exhausted() noexcept
    {
        try_write_.store( false, std::memory_order_relaxed );
    }

    /// Restore speculative reads (kernel signalled readiness via CQE).
    /// If the socket has hit perma_off_read_ the re-arm is suppressed
    /// — the strike-counter / perma-off latch overrides this signal.
    void on_async_read_ready() noexcept
    {
        if ( !perma_off_read_.load( std::memory_order_relaxed ) )
            try_read_.store( true, std::memory_order_relaxed );
    }

    /// Restore speculative writes (kernel signalled readiness via CQE).
    void on_async_write_ready() noexcept
    {
        try_write_.store( true, std::memory_order_relaxed );
    }
};

} // namespace boost::corosio::detail

#endif
