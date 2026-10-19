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
#include <list>
#include <mutex>
#include <thread>

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

/** Post probes to a pool until it refuses one, then release a blocker.

    A refused post is the only sign that the pool's shutdown has begun.
    The probes accepted before then queue harmlessly behind the rest and
    are disposed of by that shutdown, so `probes` must outlive it.

    @param pool The pool whose shutdown is awaited.
    @param blocker The item occupying the pool's only worker.
    @param probes Storage for the probes the pool accepts.
*/
inline void
release_when_refused(
    detail::thread_pool& pool,
    pool_blocker& blocker,
    std::list<detail::pool_work_item>& probes)
{
    for (;;)
    {
        auto& p    = probes.emplace_back();
        p.func_    = [](detail::pool_work_item*) noexcept {};
        p.destroy_ = [](detail::pool_work_item*) noexcept {};
        if (pool.post(&p))
            break;
        std::this_thread::yield();
    }
    blocker.release();
}

/** A service that releases a @ref pool_blocker as the context shuts down.

    Create it last so the newest-first shutdown walk runs it before any
    library service. Armed with the blocker alone, it lets the blocked
    worker go at the very top of teardown, so the operation queued
    behind it is still pending, on the pool or as a queued completion,
    while the context is being destroyed.

    Armed with the pool too, it holds the worker until the pool's own
    shutdown has begun, so the queued operation is always disposed of
    by the pool rather than run.
*/
class pool_release_gate final : public capy::execution_context::service
{
    pool_blocker* blocker_      = nullptr;
    detail::thread_pool* pool_ = nullptr;
    std::list<detail::pool_work_item> probes_;
    std::thread releaser_;

    void shutdown() override
    {
        if (!blocker_)
            return;
        if (!pool_)
        {
            blocker_->release();
            return;
        }
        // The pool is older, so it shuts down after this returns; the
        // wait for it has to happen on another thread.
        releaser_ = std::thread(
            [this] { release_when_refused(*pool_, *blocker_, probes_); });
    }

public:
    /// Destroy the gate, joining its releaser.
    ~pool_release_gate()
    {
        if (releaser_.joinable())
            releaser_.join();
    }

    /// The lookup key for this service.
    using key_type = pool_release_gate;

    /// Construct an unarmed gate.
    explicit pool_release_gate(capy::execution_context&) noexcept {}

    /** Arm the gate.

        @param b The blocker to release when the context shuts down.
        @param pool If set, release only once this pool refuses work.
    */
    void arm(pool_blocker& b, detail::thread_pool* pool = nullptr) noexcept
    {
        blocker_ = &b;
        pool_    = pool;
    }
};

/** Occupy the context's thread pool with a blocker armed for teardown.

    @param ioc The context whose pool is occupied.
    @param blocker The item to post; must outlive `ioc`.
    @param until_pool_shutdown If true, the blocker is released only
        once the pool's shutdown has begun, so everything queued behind
        it is disposed of rather than run.

    @return True if the pool accepted the item.
*/
inline bool
park_pool_worker(
    io_context& ioc,
    pool_blocker& blocker,
    bool until_pool_shutdown = false)
{
    auto* pool = ioc.find_service<detail::thread_pool>();
    if (!pool)
        return false;
    ioc.use_service<pool_release_gate>().arm(
        blocker, until_pool_shutdown ? pool : nullptr);
    return !pool->post(&blocker);
}

/** Shut down a context's pool while a blocker still holds its worker.

    Everything queued behind the blocker is then still queued when the
    pool stops taking work, so the pool disposes of it at shutdown
    instead of running it. The context itself stays alive.

    @param ioc The context whose pool is shut down.
    @param blocker The item occupying the pool's only worker.
*/
inline void
shutdown_pool_behind(io_context& ioc, pool_blocker& blocker)
{
    auto& pool = ioc.use_service<detail::thread_pool>();
    std::list<detail::pool_work_item> probes;
    std::thread stopper([&pool] { pool.shutdown(); });
    release_when_refused(pool, blocker, probes);
    stopper.join();
}

} // namespace boost::corosio::test

#endif // BOOST_COROSIO_TEST_POOL_TEARDOWN_HPP
