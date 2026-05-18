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

public:
    /// Return true when speculative read is currently worth trying.
    bool may_speculate_read() const noexcept
    {
        return try_read_.load( std::memory_order_relaxed );
    }

    /// Return true when speculative write is currently worth trying.
    bool may_speculate_write() const noexcept
    {
        return try_write_.load( std::memory_order_relaxed );
    }

    /// Disable speculative reads (kernel buffer is empty).
    void on_read_exhausted() noexcept
    {
        try_read_.store( false, std::memory_order_relaxed );
    }

    /// Disable speculative writes (kernel buffer is full).
    void on_write_exhausted() noexcept
    {
        try_write_.store( false, std::memory_order_relaxed );
    }

    /// Restore speculative reads (kernel signalled readiness via CQE).
    void on_async_read_ready() noexcept
    {
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
