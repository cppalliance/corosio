//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_THREAD_POOL_HPP
#define BOOST_COROSIO_DETAIL_THREAD_POOL_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace boost::corosio::detail {

/** Base class for thread pool work items.

    Derive from this to create work that can be posted to a
    @ref thread_pool. Uses static function pointer dispatch,
    consistent with the IOCP `op` pattern.

    @par Example
    @code
    struct my_work : pool_work_item
    {
        int* result;
        static void execute( pool_work_item* w ) noexcept
        {
            auto* self = static_cast<my_work*>( w );
            *self->result = 42;
        }
    };

    my_work w;
    w.func_ = &my_work::execute;
    w.result = &r;
    pool.post( &w );
    @endcode
*/
struct pool_work_item : intrusive_queue<pool_work_item>::node
{
    /// Static dispatch function signature.
    using func_type = void (*)(pool_work_item*) noexcept;

    /// Completion handler invoked by the worker thread.
    func_type func_ = nullptr;
};

/** Shared thread pool for dispatching blocking operations.

    Provides a fixed pool of reusable worker threads for operations
    that cannot be integrated with async I/O (e.g. blocking DNS
    calls). Registered as an `execution_context::service` so it
    is a singleton per io_context.

    Threads are created eagerly in the constructor. The default
    thread count is 1.

    @par Thread Safety
    All public member functions are thread-safe.

    @par Shutdown
    Sets a shutdown flag, notifies all threads, and joins them.
    In-flight blocking calls complete naturally before the thread
    exits.
*/
class thread_pool final
    : public capy::execution_context::service
{
    std::mutex mutex_;
    std::condition_variable cv_;
    intrusive_queue<pool_work_item> work_queue_;
    std::vector<std::thread> threads_;
    bool shutdown_ = false;

    void worker_loop();

public:
    using key_type = thread_pool;

    /** Construct the thread pool service.

        Eagerly creates all worker threads.

        @par Exception Safety
        Strong guarantee. If thread creation fails, all
        already-created threads are shut down and joined
        before the exception propagates.

        @param ctx Reference to the owning execution_context.
        @param num_threads Number of worker threads. Must be
               at least 1.

        @throws std::logic_error If `num_threads` is 0.
    */
    explicit thread_pool(
        capy::execution_context& ctx,
        unsigned num_threads = 1)
    {
        (void)ctx;
        if (!num_threads)
            throw std::logic_error(
                "thread_pool requires at least 1 thread");
        threads_.reserve(num_threads);
        try
        {
            for (unsigned i = 0; i < num_threads; ++i)
                threads_.emplace_back([this] { worker_loop(); });
        }
        catch (...)
        {
            shutdown();
            throw;
        }
    }

    ~thread_pool() override = default;

    thread_pool(thread_pool const&) = delete;
    thread_pool& operator=(thread_pool const&) = delete;

    /** Enqueue a work item for execution on the thread pool.

        Zero-allocation: the caller owns the work item's storage.

        @param w The work item to execute. Must remain valid until
                 its `func_` has been called.

        @return `true` if the item was enqueued, `false` if the
                pool has already shut down.
    */
    bool post(pool_work_item* w) noexcept;

    /** Shut down the thread pool.

        Signals all threads to exit after draining any
        remaining queued work, then joins them.
    */
    void shutdown() override;
};

inline void
thread_pool::worker_loop()
{
    for (;;)
    {
        pool_work_item* w;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return shutdown_ || !work_queue_.empty();
            });

            w = work_queue_.pop();
            if (!w)
            {
                if (shutdown_)
                    return;
                continue;
            }
        }
        w->func_(w);
    }
}

inline bool
thread_pool::post(pool_work_item* w) noexcept
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_)
            return false;
        work_queue_.push(w);
    }
    cv_.notify_one();
    return true;
}

inline void
thread_pool::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
    }
    cv_.notify_all();

    for (auto& t : threads_)
    {
        if (t.joinable())
            t.join();
    }
    threads_.clear();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (work_queue_.pop())
            ;
    }
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_DETAIL_THREAD_POOL_HPP
