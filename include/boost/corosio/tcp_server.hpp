//
// Copyright (c) 2026 Vinnie Falco (vinnie dot falco at gmail dot com)
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
#include <boost/corosio/acceptor.hpp>
#include <boost/corosio/socket.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/concept/execution_context.hpp>
#include <boost/capy/concept/io_awaitable.hpp>
#include <boost/capy/concept/executor.hpp>
#include <boost/capy/ex/any_executor.hpp>
#include <boost/capy/ex/run_async.hpp>

#include <coroutine>
#include <memory>
#include <ranges>
#include <vector>

namespace boost::corosio {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4251) // class needs to have dll-interface
#endif

/** TCP server with pooled workers.

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
    @code
    io_context ioc;
    tcp_server srv(ioc, ioc.get_executor());
    srv.set_workers(make_workers(ioc, 100));
    srv.bind(endpoint{address_v4::any(), 8080});
    srv.start();
    ioc.run();  // Blocks until all work completes
    @endcode

    @par Graceful Shutdown
    To shut down gracefully, call @ref stop then drain the io_context:
    @code
    // From a signal handler or timer callback:
    srv.stop();

    // ioc.run() returns after pending work drains.
    // Then from the thread that called ioc.run():
    srv.join();  // Wait for accept loops to finish
    @endcode

    @par Restart After Stop
    The server can be restarted after a complete shutdown cycle.
    You must drain the io_context and call @ref join before restarting:
    @code
    srv.start();
    ioc.run_for( 10s );   // Run for a while
    srv.stop();           // Signal shutdown
    ioc.run();            // REQUIRED: drain pending completions
    srv.join();           // REQUIRED: wait for accept loops

    // Now safe to restart
    srv.start();
    ioc.run();
    @endcode

    @par WARNING: What NOT to Do
    - Do NOT call @ref join from inside a worker coroutine (deadlock).
    - Do NOT call @ref join from a thread running `ioc.run()` (deadlock).
    - Do NOT call @ref start without completing @ref join after @ref stop.
    - Do NOT call `ioc.stop()` for graceful shutdown; use @ref stop instead.

    @par Example
    @code
    class my_worker : public tcp_server::worker_base
    {
        corosio::socket sock_;
        capy::any_executor ex_;
    public:
        my_worker(io_context& ctx)
            : sock_(ctx)
            , ex_(ctx.get_executor())
        {
        }

        corosio::socket& socket() override { return sock_; }

        void run(launcher launch) override
        {
            launch(ex_, [](corosio::socket* sock) -> capy::task<>
            {
                // handle connection using sock
                co_return;
            }(&sock_));
        }
    };

    auto make_workers(io_context& ctx, int n)
    {
        std::vector<std::unique_ptr<tcp_server::worker_base>> v;
        v.reserve(n);
        for(int i = 0; i < n; ++i)
            v.push_back(std::make_unique<my_worker>(ctx));
        return v;
    }

    io_context ioc;
    tcp_server srv(ioc, ioc.get_executor());
    srv.set_workers(make_workers(ioc, 100));
    @endcode

    @see worker_base, set_workers, launcher
*/
class BOOST_COROSIO_DECL
    tcp_server
{
public:
    class worker_base;  ///< Abstract base for connection handlers.
    class launcher;     ///< Move-only handle to launch worker coroutines.

private:
    struct waiter
    {
        waiter* next;
        std::coroutine_handle<> h;
        worker_base* w;
    };

    struct impl;

    static impl* make_impl(capy::execution_context& ctx);

    impl* impl_;
    capy::any_executor ex_;
    waiter* waiters_ = nullptr;
    worker_base* idle_head_ = nullptr;    // Forward list: available workers
    worker_base* active_head_ = nullptr;  // Doubly linked: workers handling connections
    worker_base* active_tail_ = nullptr;  // Tail for O(1) push_back
    std::size_t active_accepts_ = 0;      // Number of active do_accept coroutines
    std::shared_ptr<void> storage_;       // Owns the worker container (type-erased)
    bool running_ = false;

    // Idle list (forward/singly linked) - push front, pop front
    void idle_push(worker_base* w) noexcept
    {
        w->next_ = idle_head_;
        idle_head_ = w;
    }

    worker_base* idle_pop() noexcept
    {
        auto* w = idle_head_;
        if(w) idle_head_ = w->next_;
        return w;
    }

    bool idle_empty() const noexcept { return idle_head_ == nullptr; }

    // Active list (doubly linked) - push back, remove anywhere
    void active_push(worker_base* w) noexcept
    {
        w->next_ = nullptr;
        w->prev_ = active_tail_;
        if(active_tail_)
            active_tail_->next_ = w;
        else
            active_head_ = w;
        active_tail_ = w;
    }

    void active_remove(worker_base* w) noexcept
    {
        // Skip if not in active list (e.g., after failed accept)
        if(w != active_head_ && w->prev_ == nullptr)
            return;
        if(w->prev_)
            w->prev_->next_ = w->next_;
        else
            active_head_ = w->next_;
        if(w->next_)
            w->next_->prev_ = w->prev_;
        else
            active_tail_ = w->prev_;
        w->prev_ = nullptr;  // Mark as not in active list
    }

    template<capy::Executor Ex>
    struct launch_wrapper
    {
        struct promise_type
        {
            Ex ex;  // Stored directly in frame, no allocation
            std::stop_token st;

            // For regular coroutines: first arg is executor, second is stop token
            template<class E, class S, class... Args>
                requires capy::Executor<std::decay_t<E>>
            promise_type(E e, S s, Args&&...)
                : ex(std::move(e))
                , st(std::move(s))
            {
            }

            // For lambda coroutines: first arg is closure, second is executor, third is stop token
            template<class Closure, class E, class S, class... Args>
                requires (!capy::Executor<std::decay_t<Closure>> && 
                          capy::Executor<std::decay_t<E>>)
            promise_type(Closure&&, E e, S s, Args&&...)
                : ex(std::move(e))
                , st(std::move(s))
            {
            }

            launch_wrapper get_return_object() noexcept {
                return {std::coroutine_handle<promise_type>::from_promise(*this)};
            }
            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_never final_suspend() noexcept { return {}; }
            void return_void() noexcept {}
            void unhandled_exception() { std::terminate(); }

            // Pass through simple awaitables, inject executor/stop_token for IoAwaitable
            template<class Awaitable>
            auto await_transform(Awaitable&& a)
            {
                using AwaitableT = std::decay_t<Awaitable>;
                // Simple awaitable: has await_suspend(coroutine_handle<>) but not IoAwaitable
                if constexpr (
                    requires { a.await_suspend(std::declval<std::coroutine_handle<>>()); } &&
                    !capy::IoAwaitable<AwaitableT>)
                {
                    return std::forward<Awaitable>(a);
                }
                else
                {
                    struct adapter
                    {
                        AwaitableT aw;
                        Ex* ex_ptr;
                        std::stop_token* st_ptr;

                        bool await_ready() { return aw.await_ready(); }
                        decltype(auto) await_resume() { return aw.await_resume(); }

                        auto await_suspend(std::coroutine_handle<promise_type> h)
                        {
                            static_assert(capy::IoAwaitable<AwaitableT>);
                            return aw.await_suspend(h, *ex_ptr, *st_ptr);
                        }
                    };
                    return adapter{std::forward<Awaitable>(a), &ex, &st};
                }
            }
        };

        std::coroutine_handle<promise_type> h;

        launch_wrapper(std::coroutine_handle<promise_type> handle) noexcept
            : h(handle)
        {
        }

        ~launch_wrapper()
        {
            if(h)
                h.destroy();
        }

        launch_wrapper(launch_wrapper&& o) noexcept
            : h(std::exchange(o.h, nullptr))
        {
        }

        launch_wrapper(launch_wrapper const&) = delete;
        launch_wrapper& operator=(launch_wrapper const&) = delete;
        launch_wrapper& operator=(launch_wrapper&&) = delete;
    };

    // Named functor to avoid incomplete lambda type in coroutine promise
    template<class Executor>
    struct launch_coro
    {
        launch_wrapper<Executor> operator()(
            Executor,
            std::stop_token,
            tcp_server* self,
            capy::task<void> t,
            worker_base* wp)
        {
            // Executor and stop token stored in promise via constructor
            co_await std::move(t);
            co_await self->push(*wp);
        }
    };

    class push_awaitable
    {
        tcp_server& self_;
        worker_base& w_;

    public:
        push_awaitable(
            tcp_server& self,
            worker_base& w) noexcept
            : self_(self)
            , w_(w)
        {
        }

        bool await_ready() const noexcept
        {
            return false;
        }

        template<class Ex>
        std::coroutine_handle<>
        await_suspend(
            std::coroutine_handle<> h,
            Ex const&, std::stop_token) noexcept
        {
            // Dispatch to server's executor before touching shared state
            return self_.ex_.dispatch(h);
        }

        void await_resume() noexcept
        {
            // Running on server executor - safe to modify lists
            // Remove from active (if present), then wake waiter or add to idle
            self_.active_remove(&w_);
            if(self_.waiters_)
            {
                auto* wait = self_.waiters_;
                self_.waiters_ = wait->next;
                wait->w = &w_;
                self_.ex_.post(wait->h);
            }
            else
            {
                self_.idle_push(&w_);
            }
        }
    };

    class pop_awaitable
    {
        tcp_server& self_;
        waiter wait_;

    public:
        pop_awaitable(tcp_server& self) noexcept
            : self_(self)
            , wait_{}
        {
        }

        bool await_ready() const noexcept
        {
            return !self_.idle_empty();
        }

        template<class Ex>
        bool
        await_suspend(
            std::coroutine_handle<> h,
            Ex const&, std::stop_token) noexcept
        {
            // Running on server executor (do_accept runs there)
            wait_.h = h;
            wait_.w = nullptr;
            wait_.next = self_.waiters_;
            self_.waiters_ = &wait_;
            return true;
        }

        worker_base& await_resume() noexcept
        {
            // Running on server executor
            if(wait_.w)
                return *wait_.w;  // Woken by push_awaitable
            return *self_.idle_pop();
        }
    };

    push_awaitable push(worker_base& w)
    {
        return push_awaitable{*this, w};
    }

    // Synchronous version for destructor/guard paths
    // Must be called from server executor context
    void push_sync(worker_base& w) noexcept
    {
        active_remove(&w);
        if(waiters_)
        {
            auto* wait = waiters_;
            waiters_ = wait->next;
            wait->w = &w;
            ex_.post(wait->h);
        }
        else
        {
            idle_push(&w);
        }
    }

    pop_awaitable pop()
    {
        return pop_awaitable{*this};
    }

    capy::task<void> do_accept(acceptor& acc);

public:
    /** Abstract base class for connection handlers.

        Derive from this class to implement custom connection handling.
        Each worker owns a socket and is reused across multiple
        connections to avoid per-connection allocation.

        @see tcp_server, launcher
    */
    class BOOST_COROSIO_DECL
        worker_base
    {
        // Ordered largest to smallest for optimal packing
        std::stop_source stop_;        // ~16 bytes
        worker_base* next_ = nullptr;  // 8 bytes - used by idle and active lists
        worker_base* prev_ = nullptr;  // 8 bytes - used only by active list

        friend class tcp_server;

    public:
        /// Destroy the worker.
        virtual ~worker_base() = default;

        /** Handle an accepted connection.

            Called when this worker is dispatched to handle a new
            connection. The implementation must invoke the launcher
            exactly once to start the handling coroutine.

            @param launch Handle to launch the connection coroutine.
        */
        virtual void run(launcher launch) = 0;

        /// Return the socket used for connections.
        virtual corosio::socket& socket() = 0;
    };

    /** Move-only handle to launch a worker coroutine.

        Passed to @ref worker_base::run to start the connection-handling
        coroutine. The launcher ensures the worker returns to the idle
        pool when the coroutine completes or if launching fails.

        The launcher must be invoked exactly once via `operator()`.
        If destroyed without invoking, the worker is returned to the
        idle pool automatically.

        @see worker_base::run
    */
    class BOOST_COROSIO_DECL
        launcher
    {
        tcp_server* srv_;
        worker_base* w_;

        friend class tcp_server;

        launcher(tcp_server& srv, worker_base& w) noexcept
            : srv_(&srv)
            , w_(&w)
        {
        }

    public:
        /// Return the worker to the pool if not launched.
        ~launcher()
        {
            if(w_)
                srv_->push_sync(*w_);
        }

        launcher(launcher&& o) noexcept
            : srv_(o.srv_)
            , w_(std::exchange(o.w_, nullptr))
        {
        }
        launcher(launcher const&) = delete;
        launcher& operator=(launcher const&) = delete;
        launcher& operator=(launcher&&) = delete;

        /** Launch the connection-handling coroutine.

            Starts the given coroutine on the specified executor. When
            the coroutine completes, the worker is automatically returned
            to the idle pool.

            @param ex The executor to run the coroutine on.
            @param task The coroutine to execute.

            @throws std::logic_error If this launcher was already invoked.
        */
        template<class Executor>
        void operator()(Executor const& ex, capy::task<void> task)
        {
            if(! w_)
                detail::throw_logic_error(); // launcher already invoked

            auto* w = std::exchange(w_, nullptr);

            // Worker is being dispatched - add to active list
            srv_->active_push(w);

            // Return worker to pool if coroutine setup throws
            struct guard_t {
                tcp_server* srv;
                worker_base* w;
                ~guard_t() { if(w) srv->push_sync(*w); }
            } guard{srv_, w};

            // Reset worker's stop source for this connection
            w->stop_ = {};
            auto st = w->stop_.get_token();

            auto wrapper = launch_coro<Executor>{}(
                ex, st, srv_, std::move(task), w);

            // Executor and stop token stored in promise via constructor
            ex.post(std::exchange(wrapper.h, nullptr)); // Release before post
            guard.w = nullptr; // Success - dismiss guard
        }
    };

    /** Construct a TCP server.

        @tparam Ctx Execution context type satisfying ExecutionContext.
        @tparam Ex Executor type satisfying Executor.

        @param ctx The execution context for socket operations.
        @param ex The executor for dispatching coroutines.

        @par Example
        @code
        tcp_server srv(ctx, ctx.get_executor());
        srv.set_workers(make_workers(ctx, 100));
        srv.bind(endpoint{...});
        srv.start();
        @endcode
    */
    template<
        capy::ExecutionContext Ctx,
        capy::Executor Ex>
    tcp_server(Ctx& ctx, Ex ex)
        : impl_(make_impl(ctx))
        , ex_(std::move(ex))
    {
    }

public:
    ~tcp_server();
    tcp_server(tcp_server const&) = delete;
    tcp_server& operator=(tcp_server const&) = delete;
    tcp_server(tcp_server&& o) noexcept;
    tcp_server& operator=(tcp_server&& o) noexcept;

    /** Bind to a local endpoint.

        Creates an acceptor listening on the specified endpoint.
        Multiple endpoints can be bound by calling this method
        multiple times before @ref start.

        @param ep The local endpoint to bind to.

        @return The error code if binding fails.
    */
    std::error_code
    bind(endpoint ep);

    /** Set the worker pool.

        Replaces any existing workers with the given range. Any
        previous workers are released and the idle/active lists
        are cleared before populating with new workers.

        @tparam Range Forward range of pointer-like objects to worker_base.

        @param workers Range of workers to manage. Each element must
            support `std::to_address()` yielding `worker_base*`.

        @par Example
        @code
        std::vector<std::unique_ptr<my_worker>> workers;
        for(int i = 0; i < 100; ++i)
            workers.push_back(std::make_unique<my_worker>(ctx));
        srv.set_workers(std::move(workers));
        @endcode
    */
    template<std::ranges::forward_range Range>
        requires std::convertible_to<
            decltype(std::to_address(
                std::declval<std::ranges::range_value_t<Range>&>())),
            worker_base*>
    void
    set_workers(Range&& workers)
    {
        // Clear existing state
        storage_.reset();
        idle_head_ = nullptr;
        active_head_ = nullptr;
        active_tail_ = nullptr;

        // Take ownership and populate idle list
        using StorageType = std::decay_t<Range>;
        auto* p = new StorageType(std::forward<Range>(workers));
        storage_ = std::shared_ptr<void>(p, [](void* ptr) {
            delete static_cast<StorageType*>(ptr);
        });
        for(auto&& elem : *static_cast<StorageType*>(p))
            idle_push(std::to_address(elem));
    }

    /** Start accepting connections.

        Launches accept loops for all bound endpoints. Incoming
        connections are dispatched to idle workers from the pool.
        
        Calling `start()` on an already-running server has no effect.

        @par Preconditions
        - At least one endpoint bound via @ref bind.
        - Workers provided to the constructor.
        - If restarting, @ref join must have completed first.

        @par Effects
        Creates one accept coroutine per bound endpoint. Each coroutine
        runs on the server's executor, waiting for connections and
        dispatching them to idle workers.

        @par Restart Sequence
        To restart after stopping, complete the full shutdown cycle:
        @code
        srv.start();
        ioc.run_for( 1s );
        srv.stop();       // 1. Signal shutdown
        ioc.run();        // 2. Drain remaining completions
        srv.join();       // 3. Wait for accept loops

        // Now safe to restart
        srv.start();
        ioc.run();
        @endcode

        @par Thread Safety
        Not thread safe.
        
        @throws std::logic_error If a previous session has not been
            joined (accept loops still active).
    */
    void start();

    /** Stop accepting connections.

        Signals all listening ports to stop accepting new connections
        and requests cancellation of active workers via their stop tokens.
        
        This function returns immediately; it does not wait for workers
        to finish. Pending I/O operations complete asynchronously.

        Calling `stop()` on a non-running server has no effect.

        @par Effects
        - Closes all acceptors (pending accepts complete with error).
        - Requests stop on each active worker's stop token.
        - Workers observing their stop token should exit promptly.

        @par Postconditions
        No new connections will be accepted. Active workers continue
        until they observe their stop token or complete naturally.

        @par What Happens Next
        After calling `stop()`:
        1. Let `ioc.run()` return (drains pending completions).
        2. Call @ref join to wait for accept loops to finish.
        3. Only then is it safe to restart or destroy the server.

        @par Thread Safety
        Not thread safe.

        @see join, start
    */
    void stop();

    /** Block until all accept loops complete.

        Blocks the calling thread until all accept coroutines launched
        by @ref start have finished executing. This synchronizes the
        shutdown sequence, ensuring the server is fully stopped before
        restarting or destroying it.

        @par Preconditions
        @ref stop has been called and `ioc.run()` has returned.

        @par Postconditions
        All accept loops have completed. The server is in the stopped
        state and may be restarted via @ref start.

        @par Example (Correct Usage)
        @code
        // main thread
        srv.start();
        ioc.run();      // Blocks until work completes
        srv.join();     // Safe: called after ioc.run() returns
        @endcode

        @par WARNING: Deadlock Scenarios
        Calling `join()` from the wrong context causes deadlock:

        @code
        // WRONG: calling join() from inside a worker coroutine
        void run( launcher launch ) override
        {
            launch( ex, [this]() -> capy::task<>
            {
                srv_.join();  // DEADLOCK: blocks the executor
                co_return;
            }());
        }

        // WRONG: calling join() while ioc.run() is still active
        std::thread t( [&]{ ioc.run(); } );
        srv.stop();
        srv.join();  // DEADLOCK: ioc.run() still running in thread t
        @endcode

        @par Thread Safety
        May be called from any thread, but will deadlock if called
        from within the io_context event loop or from a worker coroutine.

        @see stop, start
    */
    void join();

private:
    capy::task<> do_stop();
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace boost::corosio

#endif
