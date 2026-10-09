//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TCP_SERVER_HPP
#define BOOST_COROSIO_TCP_SERVER_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/concept/execution_context.hpp>
#include <boost/capy/concept/io_awaitable.hpp>
#include <boost/capy/concept/executor.hpp>
#include <boost/capy/ex/any_executor.hpp>
#include <boost/capy/ex/frame_alloc_mixin.hpp>
#include <boost/capy/ex/frame_allocator.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/ex/recycling_memory_resource.hpp>
#include <boost/capy/ex/run_async.hpp>

#include <atomic>
#include <coroutine>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

namespace boost::corosio {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251) // class needs to have dll-interface
#endif

/** Manages a pool of reusable workers that handle incoming TCP connections.

    This class manages a pool of reusable worker objects that handle
    incoming connections. When a connection arrives, an idle worker
    is dispatched to handle it. After the connection completes, the
    worker returns to the pool for reuse, avoiding allocation overhead
    per connection.

    Workers are set via @ref set_workers as a forward range of
    pointer-like objects (e.g., `unique_ptr<worker_base>`). The server
    takes ownership of the container via type erasure.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Unsafe.

    @par Lifecycle
    The server operates in three states:

    - **Stopped**: Initial state, or after @ref join completes.
    - **Running**: After @ref start, actively accepting connections.
    - **Stopping**: After @ref stop, draining active work.

    State transitions:
    @code
    [Stopped] --start()--> [Running] --stop()--> [Stopping] --join()--> [Stopped]
    @endcode

    @par Running the Server
    @par !example running_the_server

    @par Graceful Shutdown
    To shut down gracefully, call @ref stop then drain the `io_context`:
    @par !example graceful_shutdown

    @par Restart After Stop
    The server can be restarted after a complete shutdown cycle.
    You must drain the `io_context`, call @ref join, and restart the
    `io_context` itself (`ioc.restart()`) before restarting:
    @par !example restart_after_stop

    @par WARNING: What NOT to Do
    - Do NOT call @ref join from inside a worker coroutine (deadlock).
    - Do NOT call @ref join from a thread running `ioc.run()` (deadlock).
    - Do NOT call @ref start without completing @ref join after @ref stop.
    - Do NOT call `ioc.stop()` for graceful shutdown; use @ref stop instead.

    @par Example
    @par !example custom_worker

    @see worker_base, set_workers, launcher
*/
class BOOST_COROSIO_DECL tcp_server
{
public:
    class worker_base; ///< Abstract base for connection handlers.
    class launcher;    ///< Move-only handle to launch worker coroutines.

private:
    struct waiter
    {
        waiter* next;
        capy::continuation cont;
        worker_base* w;
    };

    struct impl;

    /** Everything the server's coroutines reach.

        Accept loops, launchers and the coroutines that start
        connections each hold a reference, so moving or destroying the
        server never strands them. The lists are guarded by `mutex`,
        because workers return from whichever thread their coroutine
        finished on.
    */
    struct state
    {
        std::atomic<std::size_t> refs{1};
        impl* impl_;
        capy::any_executor ex;
        std::mutex mutex;
        waiter* waiters            = nullptr;
        worker_base* idle_head     = nullptr; // forward list
        worker_base* active_head   = nullptr; // doubly linked
        worker_base* active_tail   = nullptr;
        std::size_t active_accepts = 0; // guarded by the join mutex
        std::size_t lent           = 0; // borrowed workers, guarded by mutex
        std::shared_ptr<void> storage;  // owns the worker container
        bool running = false;
        // Set by stop(); accept loops waiting for a worker give up.
        bool stopping = false; // guarded by mutex

        state(impl* i, capy::any_executor const& e) noexcept : impl_(i), ex(e)
        {
        }

        // The list operations below require `mutex`.

        void idle_push(worker_base* w) noexcept
        {
            w->next_  = idle_head;
            idle_head = w;
        }

        worker_base* idle_pop() noexcept
        {
            auto* w = idle_head;
            if (w)
            {
                idle_head = w->next_;
                ++lent;
            }
            return w;
        }

        void active_push(worker_base* w) noexcept
        {
            w->next_ = nullptr;
            w->prev_ = active_tail;
            if (active_tail)
                active_tail->next_ = w;
            else
                active_head = w;
            active_tail = w;
        }

        void active_remove(worker_base* w) noexcept
        {
            // Skip if not in active list (e.g., after failed accept)
            if (w != active_head && w->prev_ == nullptr)
                return;
            if (w->prev_)
                w->prev_->next_ = w->next_;
            else
                active_head = w->next_;
            if (w->next_)
                w->next_->prev_ = w->prev_;
            else
                active_tail = w->prev_;
            w->prev_ = nullptr; // Mark as not in active list
        }

        /// Return @p w to the pool, handing it to a waiting accept loop
        /// if there is one.
        void push(worker_base& w) noexcept
        {
            waiter* wake = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex);
                active_remove(&w);
                if (waiters)
                {
                    wake    = waiters;
                    waiters = wake->next;
                    wake->w = &w;
                }
                else
                {
                    idle_push(&w);
                    --lent;
                }
            }
            if (wake)
                ex.post(wake->cont);
        }

        /// Drop one hold on @p w, returning it to the pool with the last.
        void drop_hold(worker_base& w) noexcept
        {
            if (w.holds_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                push(w);
        }

        /// Mark the pool as stopping and release every accept loop
        /// waiting for a worker, with none.
        void stop_waiters() noexcept
        {
            waiter* list = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex);
                stopping = true;
                list     = std::exchange(waiters, nullptr);
            }
            while (list)
            {
                auto* next = list->next;
                ex.post(list->cont);
                list = next;
            }
        }
    };

    /// Add a reference to @p s.
    static void add_ref(state* s) noexcept
    {
        s->refs.fetch_add(1, std::memory_order_relaxed);
    }

    /// Drop a reference to @p s, freeing it with the last one.
    static void release(state* s) noexcept;

    /// An owned reference to the server's state.
    class state_ref
    {
        state* p_ = nullptr;

    public:
        explicit state_ref(state* p) noexcept : p_(p)
        {
            add_ref(p_);
        }

        state_ref(state_ref const& o) noexcept : p_(o.p_)
        {
            if (p_)
                add_ref(p_);
        }

        state_ref(state_ref&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}

        state_ref& operator=(state_ref const&) = delete;
        state_ref& operator=(state_ref&&)      = delete;

        ~state_ref()
        {
            if (p_)
                release(p_);
        }

        state* operator->() const noexcept
        {
            return p_;
        }

        state& operator*() const noexcept
        {
            return *p_;
        }
    };

    static state*
    make_state(capy::execution_context& ctx, capy::any_executor const& ex);

    state* st_;

    template<capy::Executor Ex>
    struct launch_wrapper
    {
        // frame_alloc_mixin routes the frame through the thread-local
        // recycling allocator, so a warmed per-connection launch does
        // not hit the global allocator.
        struct promise_type : capy::frame_alloc_mixin
        {
            Ex ex; // Executor stored directly in frame (outlives child tasks)
            capy::io_env env_;
            /// Embedded post node: the start posts this instead of the
            /// bare handle, which would heap-allocate a wrapper op.
            capy::continuation cont_;

            // For regular coroutines: first arg is executor, second is stop token
            template<class E, class S, class... Args>
                requires capy::Executor<std::decay_t<E>>
            promise_type(E e, S s, Args&&...)
                : ex(std::move(e))
                , env_{
                      capy::executor_ref(ex), std::move(s),
                      capy::get_current_frame_allocator()}
            {
            }

            // For lambda coroutines: first arg is closure, second is executor, third is stop token
            template<class Closure, class E, class S, class... Args>
                requires(!capy::Executor<std::decay_t<Closure>> &&
                         capy::Executor<std::decay_t<E>>)
            promise_type(Closure&&, E e, S s, Args&&...)
                : ex(std::move(e))
                , env_{
                      capy::executor_ref(ex), std::move(s),
                      capy::get_current_frame_allocator()}
            {
            }

            launch_wrapper get_return_object() noexcept
            {
                return {
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
            void unhandled_exception()
            {
                // LCOV_EXCL_START: terminating by contract is not a
                // coverable outcome.
                std::terminate();
                // LCOV_EXCL_STOP
            }

            // Inject io_env for IoAwaitable
            template<capy::IoAwaitable Awaitable>
            auto await_transform(Awaitable&& a)
            {
                using AwaitableT = std::decay_t<Awaitable>;
                struct adapter
                {
                    AwaitableT aw;
                    capy::io_env const* env;

                    bool await_ready()
                    {
                        return aw.await_ready();
                    }
                    decltype(auto) await_resume()
                    {
                        return aw.await_resume();
                    }

                    auto await_suspend(std::coroutine_handle<promise_type> h)
                    {
                        return aw.await_suspend(h, env);
                    }
                };
                return adapter{std::forward<Awaitable>(a), &env_};
            }
        };

        std::coroutine_handle<promise_type> h;

        launch_wrapper(std::coroutine_handle<promise_type> handle) noexcept
            : h(handle)
        {
        }

        ~launch_wrapper()
        {
            if (h)
                h.destroy();
        }

        launch_wrapper(launch_wrapper&& o) noexcept
            : h(std::exchange(o.h, nullptr))
        {
        }

        launch_wrapper(launch_wrapper const&)            = delete;
        launch_wrapper& operator=(launch_wrapper const&) = delete;
        launch_wrapper& operator=(launch_wrapper&&)      = delete;
    };

    // Named functor to avoid incomplete lambda type in coroutine promise
    template<class Executor>
    struct launch_coro
    {
        launch_wrapper<Executor> operator()(
            Executor,
            std::stop_token,
            state_ref st,
            capy::task<void> t,
            worker_base* wp)
        {
            // Executor and stop token stored in promise via constructor
            co_await std::move(t);
            co_await push_awaitable{*st, *wp}; // worker back to the pool
        }
    };

    class push_awaitable
    {
        state& st_;
        worker_base& w_;
        capy::continuation cont_;

    public:
        push_awaitable(state& st, worker_base& w) noexcept : st_(st), w_(w) {}

        bool await_ready() const noexcept
        {
            return false;
        }

        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<> h, capy::io_env const*) noexcept
        {
            // Symmetric transfer to server's executor
            cont_.h = h;
            return st_.ex.dispatch(cont_);
        }

        void await_resume() noexcept
        {
            st_.drop_hold(w_);
        }
    };

    class pop_awaitable
    {
        state& st_;
        waiter wait_;

    public:
        pop_awaitable(state& st) noexcept : st_(st), wait_{} {}

        bool await_ready() const noexcept
        {
            return false;
        }

        bool
        await_suspend(std::coroutine_handle<> h, capy::io_env const*) noexcept
        {
            std::lock_guard<std::mutex> lock(st_.mutex);
            if (st_.stopping)
            {
                wait_.w = nullptr;
                return false;
            }
            if (auto* w = st_.idle_pop())
            {
                wait_.w = w;
                return false;
            }
            wait_.cont.h = h;
            wait_.w      = nullptr;
            wait_.next   = st_.waiters;
            st_.waiters  = &wait_;
            return true;
        }

        /// Return the worker, or null once the server is stopping.
        worker_base* await_resume() noexcept
        {
            // Set by await_suspend, or by the push that woke us.
            return wait_.w;
        }
    };

    static capy::task<void> do_accept(state_ref st, tcp_acceptor& acc);
    static capy::task<> do_stop(state_ref st);

public:
    /** Handles one accepted connection using a socket the derived class owns.

        Derive from this class to implement custom connection handling.
        Each worker owns a socket and is reused across multiple
        connections to avoid per-connection allocation.

        @par Thread Safety
        run() and socket() execute on the server's executor.

        @see tcp_server, launcher
    */
    class BOOST_COROSIO_DECL worker_base
    {
        // Ordered largest to smallest for optimal packing
        std::stop_source stop_;       // ~16 bytes
        worker_base* next_ = nullptr; // 8 bytes - used by idle and active lists
        worker_base* prev_ = nullptr; // 8 bytes - used only by active list
        // One for the accept loop until run() returns, one for the
        // launcher and its connection; the last one returns the worker.
        std::atomic<unsigned> holds_{0};

        friend class tcp_server;

    public:
        /// Construct a worker.
        worker_base();

        /// Destroy the worker.
        virtual ~worker_base();

        /** Handle an accepted connection.

            Called when this worker is dispatched to handle a new
            connection. The implementation must invoke the launcher
            exactly once to start the handling coroutine.

            @par Exception Safety
            An exception escaping this function ends the process. A
            worker that cannot start the connection should handle the
            failure here; destroying the launcher without invoking it
            returns the worker to the pool.

            The worker is not given another connection before this
            returns, even if the connection it started has finished.

            @param launch Handle to start the connection coroutine.
        */
        virtual void run(launcher launch) = 0;

        /// Return the socket used for connections.
        virtual corosio::tcp_socket& socket() = 0;
    };

    /** Starts a worker's connection-handling coroutine and returns the
        worker to the idle pool automatically.

        Passed to @ref worker_base::run to start the connection-handling
        coroutine. The launcher ensures the worker returns to the idle
        pool when the coroutine completes or if starting fails.

        The launcher must be invoked exactly once via `operator()`.
        If destroyed without invoking, the worker is returned to the
        idle pool automatically.

        @see worker_base::run
    */
    class BOOST_COROSIO_DECL launcher
    {
        state* st_;
        worker_base* w_;

        friend class tcp_server;

        launcher(state& st, worker_base& w) noexcept : st_(&st), w_(&w)
        {
            add_ref(st_);
        }

    public:
        /// Return the worker to the pool if not started.
        ~launcher()
        {
            if (w_)
                st_->drop_hold(*w_);
            if (st_)
                release(st_);
        }

        /** Move construct, transferring the borrowed worker.

            @param o The launcher to take the worker from. It is left
            holding none, so only one of the two returns it.
        */
        launcher(launcher&& o) noexcept
            : st_(std::exchange(o.st_, nullptr))
            , w_(std::exchange(o.w_, nullptr))
        {
        }
        /// Copy construction is disabled; a launcher holds a borrowed worker it must return exactly once.
        launcher(launcher const&) = delete;
        /// Copy assignment is disabled; a launcher holds a borrowed worker it must return exactly once.
        launcher& operator=(launcher const&) = delete;
        /// Move assignment is disabled; a launcher is moved, never reassigned.
        launcher& operator=(launcher&&) = delete;

        /** Start the connection-handling coroutine.

            Starts the given coroutine on the specified executor. When
            the coroutine completes, the worker is automatically returned
            to the idle pool.

            @pre @p task was created on a chain whose frame allocator
                outlives the connection. The task's frame is allocated
                where it is created, and it lives until the connection
                ends. Inside @ref worker_base::run that allocator is the
                accept loop's, the frame allocator of the server's
                execution context. By default that is the process-wide
                recycling resource, so a task created there meets this.

            @tparam Executor Executor type satisfying capy::Executor.

            @param ex The executor to run the coroutine on.
            @param task The coroutine to execute.

            @throws std::logic_error If this launcher was already invoked.
        */
        template<class Executor>
        void operator()(Executor const& ex, capy::task<void> task)
        {
            if (!w_)
                detail::throw_logic_error(); // launcher already invoked

            auto* w = std::exchange(w_, nullptr);

            // A stop_source allocates shared state on construction;
            // reuse the worker's across connections and replace it
            // only once a stop has actually been delivered (the state
            // is then latched for good). Replaced before the worker is
            // listed active, under the lock stop() copies it under, so
            // that copy never races the replacement or misses it.
            std::stop_token st;
            bool stopped_before = false;
            {
                std::lock_guard<std::mutex> lock(st_->mutex);
                if (w->stop_.stop_requested())
                    w->stop_ = {};
                st = w->stop_.get_token();
                st_->active_push(w);
                stopped_before = st_->stopping;
            }
            // A stop that came before this launch never saw the worker
            // listed. The token is not handed out yet, so no callback
            // runs here.
            if (stopped_before)
                w->stop_.request_stop();

            // Return worker to pool if coroutine setup throws
            struct guard_t
            {
                state* st;
                worker_base* w;
                ~guard_t()
                {
                    if (w)
                        st->drop_hold(*w);
                }
            } guard{st_, w};

            // The connection can outlive whatever chain invoked this
            // launcher, so its frame comes from the process-wide
            // recycling resource rather than that chain's allocator.
            struct allocator_scope
            {
                std::pmr::memory_resource* prev =
                    capy::get_current_frame_allocator();
                allocator_scope() noexcept
                {
                    capy::set_current_frame_allocator(
                        capy::get_recycling_memory_resource());
                }
                ~allocator_scope()
                {
                    capy::set_current_frame_allocator(prev);
                }
            };
            launch_wrapper<Executor> wrapper = [&] {
                allocator_scope scope;
                return launch_coro<Executor>{}(
                    ex, st, state_ref(st_), std::move(task), w);
            }();

            // Executor and stop token stored in promise via
            // constructor. Post through the frame-embedded
            // continuation — posting the bare handle would allocate a
            // wrapper op. The frame stays suspended until the
            // executor resumes it, so the node outlives the queue.
            auto h = std::exchange(wrapper.h, nullptr); // Release before post
            h.promise().cont_.h = h;
            ex.post(h.promise().cont_);
            guard.w = nullptr; // Success - dismiss guard
        }
    };

    /** Construct a TCP server.

        @tparam Ctx Execution context type satisfying ExecutionContext.
        @tparam Ex Executor type satisfying Executor.

        @param ctx The execution context for socket operations.
        @param ex The executor for dispatching coroutines.

        @par Example
        @par !example tcp_server
    */
    template<capy::ExecutionContext Ctx, capy::Executor Ex>
    tcp_server(Ctx& ctx, Ex ex)
        : st_(make_state(ctx, capy::any_executor(std::move(ex))))
    {
    }

public:
    /** Destroy the server, stopping it.

        A running server is stopped and its listeners closed, so its
        accept loops end. The loops and any active workers finish on
        the server's executor without reaching the destroyed object;
        the workers stay alive until they do.

        @par Thread Safety
        Not thread safe.
    */
    ~tcp_server();

    /// Copy construction is disabled; the server owns its worker storage.
    tcp_server(tcp_server const&) = delete;
    /// Copy assignment is disabled; the server owns its worker storage.
    tcp_server& operator=(tcp_server const&) = delete;

    /** Move construct from another server.

        @param o The source server. After the move, @p o is
            in a valid but unspecified state.
    */
    tcp_server(tcp_server&& o) noexcept;

    /** Move assign from another server.

        @param o The source server. After the move, @p o is
            in a valid but unspecified state.

        @return `*this`.
    */
    tcp_server& operator=(tcp_server&& o) noexcept;

    /** Bind to a local endpoint.

        Creates an acceptor listening on the specified endpoint.
        Multiple endpoints can be bound by calling this method
        multiple times before @ref start.

        @pre No session is active: the server was never started, or
            it was stopped and joined.

        @param ep The local endpoint to bind to.

        @return An error code indicating success, or the reason binding
            failed.
    */
    [[nodiscard]] std::error_code bind(endpoint ep);

    /** Set the worker pool.

        Replaces any existing workers with the given range. Any
        previous workers are released and the idle/active lists
        are cleared before populating with new workers.

        @pre No worker is waiting on or serving a connection: the
            server was never started, or it was stopped and joined
            and every connection has finished.

        @par Exception Safety
        The server keeps its current workers if this throws.

        @throws std::logic_error If a worker is waiting on or serving
            a connection.

        @tparam Range Forward range of pointer-like objects to worker_base.

        @param workers Range of workers to manage. Each element must
            support `std::to_address()` yielding `worker_base*`.

        @par Example
        @par !example set_workers
    */
    template<std::ranges::forward_range Range>
        requires std::convertible_to<
            decltype(std::to_address(
                std::declval<std::ranges::range_value_t<Range>&>())),
            worker_base*>
    void set_workers(Range&& workers)
    {
        // The previous workers are released after the lock.
        std::shared_ptr<void> previous;
        std::lock_guard<std::mutex> lock(st_->mutex);
        if (st_->lent != 0)
            detail::throw_logic_error(
                "tcp_server::set_workers: a worker is still in use");

        using StorageType = std::decay_t<Range>;
        auto* p           = new StorageType(std::forward<Range>(workers));
        std::shared_ptr<void> storage(
            p, [](void* ptr) { delete static_cast<StorageType*>(ptr); });

        st_->idle_head   = nullptr;
        st_->active_head = nullptr;
        st_->active_tail = nullptr;
        previous         = std::exchange(st_->storage, std::move(storage));
        for (auto&& elem : *p)
            st_->idle_push(std::to_address(elem));
    }

    /** Start accepting connections.

        Starts accept loops for all bound endpoints. Incoming
        connections are dispatched to idle workers from the pool.
        
        Calling `start()` on an already-running server has no effect.

        @pre At least one endpoint bound via @ref bind.
        @pre Workers provided via @ref set_workers.
        @pre If restarting, @ref join must have completed first, and the
            `io_context` must be restarted (`ioc.restart()`).

        @par Effects
        Creates one accept coroutine per bound endpoint. Each coroutine
        runs on the server's executor, waiting for connections and
        dispatching them to idle workers.

        @par Restart Sequence
        To restart after stopping, complete the full shutdown cycle:
        @par !example start

        @par Thread Safety
        Not thread safe.
        
        @throws std::logic_error If a previous session has not been
            joined (accept loops still active).
    */
    void start();

    /** Return the local endpoint for the i-th bound port.

        @param index Zero-based index into the list of bound ports.

        @return The local endpoint, or a default-constructed endpoint
            if @p index is out of range or the acceptor is not open.
    */
    endpoint local_endpoint(std::size_t index = 0) const noexcept;

    /** Stop accepting connections.

        Requests the accept loops' stop token and requests cancellation
        of active workers via their stop tokens. The acceptors are not
        closed. A suspended accept completes once more before its loop
        observes the stop token and ends.

        This function returns immediately; it does not wait for workers
        to finish. Pending I/O operations complete asynchronously.

        Calling `stop()` on a non-running server has no effect.

        @par Effects
        - Requests stop on the accept loops' stop token. The acceptors
          are not closed; a pending accept completes once more before
          the accept loop ends.
        - Requests stop on each active worker's stop token.
        - Workers observing their stop token should exit promptly.
        - A worker reuses its stop token across connections until a stop
          is requested. Work that keeps a connection's token past that
          connection's end therefore also observes this stop.

        @par Postconditions
        The server accepts no new connections. Active workers continue
        until they observe their stop token or complete naturally.

        @par What Happens Next
        After calling `stop()`:
        1. Let `ioc.run()` return (drains pending completions).
        2. Call @ref join to wait for accept loops to finish.
        3. Only then may the server be restarted. It may be destroyed at
           any time; see @ref ~tcp_server.

        @par Thread Safety
        Not thread safe.

        @see join, start
    */
    void stop();

    /** Block until all accept loops complete.

        Blocks the calling thread until all accept coroutines started
        by @ref start have finished executing. This synchronizes the
        shutdown sequence, ensuring the server is fully stopped before
        restarting or destroying it.

        @pre @ref stop was called and `ioc.run()` returned.

        @par Postconditions
        All accept loops have completed. The server is in the stopped
        state and may be restarted via @ref start.

        @par Example (Correct Usage)
        @par !example correct_usage

        @par WARNING: Deadlock Scenario
        Calling `join()` from inside a worker coroutine deadlocks:

        @par !example deadlock_scenarios

        @par Thread Safety
        May be called from any thread. It deadlocks if called
        from within the `io_context` event loop or from a worker coroutine.

        @see stop, start
    */
    void join();

private:
    void discard() noexcept;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace boost::corosio

#endif
