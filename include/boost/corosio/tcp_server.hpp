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
#include <boost/corosio/acceptor.hpp>
#include <boost/corosio/socket.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/concept/io_awaitable.hpp>
#include <boost/capy/concept/executor.hpp>
#include <boost/capy/ex/any_executor.hpp>
#include <boost/capy/ex/run_async.hpp>

#include <coroutine>
#include <memory>
#include <stdexcept>
#include <vector>

namespace boost::corosio {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4251) // class needs to have dll-interface
#endif

/** Base class for building TCP servers with pooled workers.

    This class manages a pool of reusable worker objects that handle
    incoming connections. When a connection arrives, an idle worker
    is dispatched to handle it. After the connection completes, the
    worker returns to the pool for reuse, avoiding allocation overhead
    per connection.

    Derived classes create workers via the protected `wv_` member and
    implement custom connection handling by deriving from @ref worker_base.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Unsafe.

    @par Example
    @code
    class my_server : public tcp_server
    {
        class my_worker : public worker_base
        {
            corosio::socket sock_;
        public:
            my_worker( io_context& ctx ) : sock_( ctx ) {}
            corosio::socket& socket() override { return sock_; }

            void run( launcher launch ) override
            {
                launch( ex, [this]() -> capy::task<>
                {
                    // handle connection using sock_
                    co_return;
                }());
            }
        };

    public:
        my_server( io_context& ctx, capy::any_executor ex )
            : tcp_server( ctx, ex )
        {
            wv_.reserve( 100 );
            for( int i = 0; i < 100; ++i )
                wv_.emplace<my_worker>( ctx );
        }
    };
    @endcode

    @see worker_base, workers, launcher
*/
class BOOST_COROSIO_DECL
    tcp_server
{
public:
    class worker_base;  ///< Abstract base for connection handlers.
    class launcher;     ///< Move-only handle to launch worker coroutines.
    class workers;      ///< Container managing the worker pool.

private:
    struct waiter;

    io_context& ctx_;
    capy::any_executor ex_;
    waiter* waiters_ = nullptr;
    std::vector<acceptor> ports_;

    template<capy::Executor Ex>
    struct launch_wrapper
    {
        struct promise_type
        {
            Ex ex;  // Stored directly in frame, no allocation

            // For regular coroutines: first arg is the executor
            template<class E, class... Args>
                requires capy::Executor<std::decay_t<E>>
            promise_type(E e, Args&&...)
                : ex(std::move(e))
            {
            }

            // For lambda coroutines: first arg is lambda closure, second is executor
            template<class Closure, class E, class... Args>
                requires (!capy::Executor<std::decay_t<Closure>> && 
                          capy::Executor<std::decay_t<E>>)
            promise_type(Closure&&, E e, Args&&...)
                : ex(std::move(e))
            {
            }

            launch_wrapper get_return_object() noexcept {
                return {std::coroutine_handle<promise_type>::from_promise(*this)};
            }
            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_never final_suspend() noexcept { return {}; }
            void return_void() noexcept {}
            void unhandled_exception() { std::terminate(); }

            // Injects executor for affinity-aware awaitables
            template<class Awaitable>
            auto await_transform(Awaitable&& a)
            {
                struct adapter
                {
                    std::decay_t<Awaitable> aw;
                    Ex* ex_ptr;

                    bool await_ready() { return aw.await_ready(); }
                    auto await_resume() { return aw.await_resume(); }

                    auto await_suspend(std::coroutine_handle<promise_type> h)
                    {
                        static_assert(capy::IoAwaitable<std::decay_t<Awaitable>>);
                        return aw.await_suspend(h, *ex_ptr, std::stop_token{});
                    }
                };
                return adapter{std::forward<Awaitable>(a), &ex};
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

    struct waiter
    {
        waiter* next;
        std::coroutine_handle<> h;
        worker_base* w;
    };

    class BOOST_COROSIO_DECL
        push_awaitable
    {
        tcp_server& self_;
        worker_base& w_;

    public:
        push_awaitable(tcp_server& self, worker_base& w) noexcept;
        bool await_ready() const noexcept;

        template<typename Ex>
        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<> h, Ex const&, std::stop_token) noexcept
        {
            // Dispatch to server's executor before touching shared state
            return self_.ex_.dispatch(h);
        }

        void await_resume() noexcept;

    private:
        std::coroutine_handle<> await_suspend_impl(std::coroutine_handle<> h) noexcept;
    };

    class BOOST_COROSIO_DECL
        pop_awaitable
    {
        tcp_server& self_;
        waiter wait_;

    public:
        pop_awaitable(tcp_server& self) noexcept;
        bool await_ready() const noexcept;

        template<typename Ex>
        bool
        await_suspend(std::coroutine_handle<> h, Ex const&, std::stop_token) noexcept
        {
            wait_.h = h;
            wait_.w = nullptr;
            wait_.next = self_.waiters_;
            self_.waiters_ = &wait_;
            return true;
        }

        system::result<worker_base&> await_resume() noexcept;

    private:
        bool await_suspend_impl(std::coroutine_handle<> h) noexcept;
    };

    push_awaitable push(worker_base& w);

    void push_sync(worker_base& w) noexcept;

    pop_awaitable pop();

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
        worker_base* next = nullptr;

        friend class tcp_server;
        friend class workers;

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

    /** Container managing the worker pool.

        This container owns the worker objects and maintains an idle
        list for fast dispatch. Workers are created during server
        construction and reused across connections.
    */
    class BOOST_COROSIO_DECL
        workers
    {
        friend class tcp_server;

        std::vector<std::unique_ptr<worker_base>> v_;
        worker_base* idle_ = nullptr;

    public:
        /// Construct an empty worker pool.
        workers() = default;
        workers(workers const&) = delete;
        workers& operator=(workers const&) = delete;
        workers(workers&&) = default;
        workers& operator=(workers&&) = default;

    private:
        void push(worker_base& w) noexcept
        {
            w.next = idle_;
            idle_ = &w;
        }

        worker_base* try_pop() noexcept
        {
            auto* w = idle_;
            idle_ = w->next;
            return w;
        }

    public:
        /** Construct a worker in place and add it to the pool.

            The worker is constructed with the given arguments and
            immediately added to the idle list.

            @tparam T The worker type, must derive from @ref worker_base.

            @param args Arguments forwarded to the worker constructor.

            @return Reference to the newly created worker.
        */
        template<class T, class... Args>
        T& emplace(Args&&... args)
        {
            auto p = std::make_unique<T>(std::forward<Args>(args)...);
            auto* raw = p.get();
            v_.push_back(std::move(p));
            push(*raw);
            return static_cast<T&>(*raw);
        }

        /// Reserve capacity for `n` workers.
        void reserve(std::size_t n) { v_.reserve(n); }

        /// Return the total number of workers in the pool.
        std::size_t size() const noexcept { return v_.size(); }
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
                throw std::logic_error("launcher already invoked");

            auto* w = std::exchange(w_, nullptr);

            // Return worker to pool if coroutine setup throws
            struct guard_t {
                tcp_server* srv;
                worker_base* w;
                ~guard_t() { if(w) srv->push_sync(*w); }
            } guard{srv_, w};

            auto wrapper =
                [](Executor ex, tcp_server* self, capy::task<void> t, worker_base* wp)
                    -> launch_wrapper<Executor>
                {
                    (void)ex; // Executor stored in promise via constructor
                    co_await std::move(t);
                    co_await self->push(*wp);
                }(ex, srv_, std::move(task), w);

            // Executor is now stored in promise via constructor
            ex.post(std::exchange(wrapper.h, nullptr)); // Release before post
            guard.w = nullptr; // Success - dismiss guard
        }
    };

protected:
    workers wv_;  ///< Worker pool, populated by derived classes.

    /** Construct a TCP server.

        Derived classes call this constructor to initialize the server
        with an I/O context and executor.

        @param ctx The I/O context for socket operations.
        @param ex The executor for dispatching coroutines.
    */
    template<capy::Executor Ex>
    tcp_server(
        io_context& ctx,
        Ex const& ex)
        : ctx_(ctx)
        , ex_(ex)
    {
    }

public:
    /** Bind to a local endpoint.

        Creates an acceptor listening on the specified endpoint.
        Multiple endpoints can be bound by calling this method
        multiple times before @ref start.

        @param ep The local endpoint to bind to.

        @return The error code if binding fails.
    */
    system::error_code
    bind(endpoint ep);

    /** Start accepting connections.

        Launches accept loops for all bound endpoints. Incoming
        connections are dispatched to idle workers from the pool.

        @par Preconditions
        At least one endpoint has been bound via @ref bind.
        Workers have been added to the pool via `wv_.emplace()`.
    */
    void start();
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace boost::corosio

#endif
