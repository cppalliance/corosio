//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_POOL_TEARDOWN_HPP
#define BOOST_COROSIO_TEST_POOL_TEARDOWN_HPP

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/detail/thread_pool.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <condition_variable>
#include <mutex>

namespace boost::corosio::test {

/** A pool work item that occupies a worker until it is released.

    Post one of these to a context's thread pool and every item posted
    after it stays queued behind it. That is what holds a real
    operation's pool work undone across the context's destruction,
    without asking the test to time anything.

    Declare the blocker before the context: the pool joins its workers
    while the context is being destroyed, so the item has to outlive it.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Safe.
*/
class pool_blocker : public detail::pool_work_item
{
    std::mutex mutex_;
    std::condition_variable cv_;
    bool released_ = false;

    static void run(detail::pool_work_item* w) noexcept
    {
        auto* self = static_cast<pool_blocker*>(w);
        std::unique_lock<std::mutex> lock(self->mutex_);
        self->cv_.wait(lock, [self] { return self->released_; });
    }

public:
    /// Construct an item that blocks the worker running it.
    pool_blocker() noexcept
    {
        func_ = &pool_blocker::run;
    }

    /// Let the occupied worker go.
    void release()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        cv_.notify_one();
    }
};

/** A service that releases a @ref pool_blocker as the context shuts down.

    Create it last so the newest-first shutdown walk runs it before any
    library service: the blocked worker is let go at the very top of
    teardown, and the operation queued behind it therefore completes
    while the context is being destroyed rather than before.
*/
class pool_release_gate final
    : public capy::execution_context::service
{
    pool_blocker* blocker_ = nullptr;

    void shutdown() override
    {
        if (blocker_)
            blocker_->release();
    }

public:
    /// The lookup key for this service.
    using key_type = pool_release_gate;

    /// Construct an unarmed gate.
    explicit pool_release_gate(capy::execution_context&) noexcept {}

    /** Arm the gate.

        @param b The blocker to release when the context shuts down.
    */
    void arm(pool_blocker& b) noexcept
    {
        blocker_ = &b;
    }
};

/** Occupy the context's thread pool with a blocker armed for teardown.

    @param ioc The context whose pool is occupied.
    @param blocker The item to post; must outlive `ioc`.

    @return True if the pool accepted the item.
*/
inline bool
park_pool_worker(io_context& ioc, pool_blocker& blocker)
{
    auto* pool = ioc.find_service<detail::thread_pool>();
    if (!pool)
        return false;
    ioc.use_service<pool_release_gate>().arm(blocker);
    return !pool->post(&blocker);
}

} // namespace boost::corosio::test

#endif // BOOST_COROSIO_TEST_POOL_TEARDOWN_HPP
