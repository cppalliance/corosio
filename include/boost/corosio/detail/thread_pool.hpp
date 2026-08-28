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
#include <boost/capy/error.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/test/thread_name.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <system_error>
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
    auto ec = pool.post( &w );
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

    The service is created with its context, but the workers start on
    the first `post()`: a context that never opens a file and never
    resolves a name never pays for a thread. The default thread count
    is 1.

    @par Thread Safety
    All public member functions are thread-safe.

    @par Shutdown
    Sets a shutdown flag, notifies all threads, and joins them.
    In-flight blocking calls complete naturally before the thread
    exits.

    @note Create this service after the scheduler its work items post
    completions to. Services shut down newest first, so a pool created
    earlier joins its workers only after the scheduler has drained its
    completion queue, and the completion the last worker posts is then
    neither run nor destroyed.

    @note The type is symbol-visible because services are keyed by type
    identity: with RTTI, hidden behind a shared library boundary, a
    module that asks for the pool would look up, and create, one of its
    own (the no-RTTI key is a template static whose visibility follows
    the template it is instantiated from).
*/
class BOOST_COROSIO_SYMBOL_VISIBLE thread_pool final
    : public capy::execution_context::service
{
    std::mutex mutex_;
    std::condition_variable cv_;
    intrusive_queue<pool_work_item> work_queue_;
    std::vector<std::thread> threads_;
    unsigned num_threads_;
    bool shutdown_ = false;

    void worker_loop(unsigned index);
    std::error_code start_workers() noexcept;

public:
    using key_type = thread_pool;

    /** Construct the thread pool service.

        Records the worker count. The workers themselves start on the
        first `post()`.

        @par Exception Safety
        Strong guarantee.

        @param ctx Reference to the owning execution_context.
        @param num_threads Number of worker threads. Must be
               at least 1.

        @throws std::logic_error If `num_threads` is 0.
    */
    explicit thread_pool(
        [[maybe_unused]] capy::execution_context& ctx,
        unsigned num_threads = 1)
        : num_threads_(num_threads)
    {
        if (!num_threads)
            throw std::logic_error("thread_pool requires at least 1 thread");
    }

    /** Destroy the pool, joining any worker `shutdown()` never reached.

        The context's shutdown walk is the normal path; this only
        catches a pool created after that walk, whose `shutdown()` is
        therefore never called and whose joinable threads would
        otherwise terminate the process. A pool that was never posted
        to holds no thread and needs neither.
    */
    ~thread_pool() override
    {
        if (!threads_.empty())
            shutdown();
    }

    thread_pool(thread_pool const&)            = delete;
    thread_pool& operator=(thread_pool const&) = delete;

    /** Enqueue a work item for execution on the thread pool.

        The first item posted starts the workers. Zero-allocation:
        the caller owns the work item's storage.

        A refusal answers with the code the caller reports for the
        operation it was starting, so that a system that will not give
        the pool a thread is not mistaken for a cancellation.

        @par Thread Safety
        Safe. Racing first posts start the workers once.

        @param w The work item to execute. Must remain valid until
                 its `func_` has been called.

        @return An empty code if the item was enqueued;
                `capy::error::canceled` if the pool has already shut
                down; otherwise the code of the thread the system
                refused, which left the pool with no worker at all.
    */
    [[nodiscard]] std::error_code post(pool_work_item* w) noexcept;

    /** Return the number of workers the pool has started.

        Zero until the first `post()`, and zero again once
        `shutdown()` has joined them.

        @par Thread Safety
        Safe.
    */
    unsigned worker_count() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<unsigned>(threads_.size());
    }

    /** Shut down the thread pool.

        Signals all threads to exit after draining any
        remaining queued work, then joins them.
    */
    void shutdown() override;
};

inline void
thread_pool::worker_loop(unsigned index)
{
    // Name format chosen to fit Linux's 15-char pthread limit:
    // "tpool-svc-" (10) + up to 4 digit index leaves "tpool-svc-9999".
    char name[16];
    std::snprintf(name, sizeof(name), "tpool-svc-%u", index);
    capy::set_current_thread_name(name);

    for (;;)
    {
        pool_work_item* w;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(
                lock, [this] { return shutdown_ || !work_queue_.empty(); });

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

// Called with mutex_ held, so the workers are started once however
// many threads race the first post.
inline std::error_code
thread_pool::start_workers() noexcept
{
    if (!threads_.empty())
        return {};
    std::error_code ec;
    try
    {
        threads_.reserve(num_threads_);
        for (unsigned i = 0; i < num_threads_; ++i)
            threads_.emplace_back([this, i] { worker_loop(i + 1); });
    }
    catch (std::system_error const& e)
    {
        // The refusal is carried out, not swallowed: a thread the
        // system will not give is a real error and the operation that
        // asked for it says so, rather than reporting the cancellation
        // that belongs to a stop token.
        ec = e.code();
    }
    catch (...)
    {
        ec = std::make_error_code(std::errc::resource_unavailable_try_again);
    }
    // A pool short of workers still runs everything posted to it, only
    // less of it at once, so a partial start is a start. What it does
    // not do is come back for the rest: the size is a tuning knob, and
    // topping it up would put a thread creation on the initiator's
    // path for every operation after a refusal.
    if (!threads_.empty())
        return {};
    return ec;
}

inline std::error_code
thread_pool::post(pool_work_item* w) noexcept
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_)
            return capy::error::canceled;
        // The system can refuse a thread, and an initiator has no way
        // to throw; a refused post is the failure the callers already
        // report through the operation they were starting.
        if (auto ec = start_workers())
            return ec;
        work_queue_.push(w);
    }
    cv_.notify_one();
    return {};
}

inline void
thread_pool::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
    }
    cv_.notify_all();

    // Unlocked, though a post may add to threads_: the flag above is
    // published under the same mutex, so a post that has not taken it
    // yet will find it set and start nothing, and one already inside
    // released the mutex before this thread acquired it.
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

/** A reference to the context's shared thread pool, bound on first use.

    Services that hand blocking work to the pool hold one of these
    instead of a reference bound at construction. They are constructed
    from the scheduler's constructor, where the pool they created would
    be older than the scheduler and would join too late; binding on
    first use puts the pool after it instead.

    The owning `io_context` creates the pool service during
    construction, so by the time any operation can run the binding only
    ever finds it. That is what keeps `get()` from constructing
    anything on an initiator's thread, and so from throwing where an
    initiator may not: the throwing spelling exists for a scheduler
    driven without an `io_context`. What the service defers is its
    workers, and those are started by `post()`, which reports a refusal
    rather than throwing it.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Safe.

    @see thread_pool
*/
class thread_pool_ref
{
    capy::execution_context& ctx_;
    std::atomic<thread_pool*> pool_{nullptr};

public:
    /** Construct a reference into the given context.

        @param ctx The context whose pool is used.
    */
    explicit thread_pool_ref(capy::execution_context& ctx) noexcept
        : ctx_(ctx)
    {
    }

    thread_pool_ref(thread_pool_ref const&)            = delete;
    thread_pool_ref& operator=(thread_pool_ref const&) = delete;

    /** Return the pool, creating it if this is the first use.

        @par Preconditions
        For the throwing clauses below to be unreachable, the owning
        context must already hold the pool service. Every `io_context`
        constructor installs it — what waits for a first post is the
        service's workers, not the service — so the creating branch is
        reached only by a scheduler driven without one.

        @par Exception Safety
        Strong guarantee.

        @throws std::bad_alloc If the service cannot be allocated.

        @throws std::logic_error If the pool is asked for zero threads.

        @return The context's shared thread pool.
    */
    thread_pool& get()
    {
        auto* p = pool_.load(std::memory_order_acquire);
        if (!p)
        {
            p = &ctx_.use_service<thread_pool>();
            pool_.store(p, std::memory_order_release);
        }
        return *p;
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_DETAIL_THREAD_POOL_HPP
