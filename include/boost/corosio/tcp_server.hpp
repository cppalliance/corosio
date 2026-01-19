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
#include <boost/capy/ex/any_executor_ref.hpp>
#include <boost/capy/ex/get_stop_token.hpp>
#include <boost/capy/ex/run_async.hpp>

#include <coroutine>
#include <memory>
#include <stdexcept>
#include <vector>

namespace boost {
namespace corosio {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4251) // class needs to have dll-interface
#endif

class BOOST_COROSIO_DECL
    tcp_server
{
protected:
    class worker_base;
    class launcher;
    class workers;

private:
    struct waiter;
    class push_aw;
    class pop_aw;

    struct launch_wrapper
    {
        struct promise_type
        {
            capy::any_executor_ref d;

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
                    capy::any_executor_ref d;

                    bool await_ready() { return aw.await_ready(); }
                    auto await_resume() { return aw.await_resume(); }

                    auto await_suspend(std::coroutine_handle<promise_type> h)
                    {
                        if constexpr (capy::IoAwaitable<
                                std::decay_t<Awaitable>, capy::any_executor_ref>)
                            return aw.await_suspend(h, d, std::stop_token{});
                        else
                            return aw.await_suspend(h);
                    }
                };
                return adapter{std::forward<Awaitable>(a), d};
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

    io_context& ctx_;
    capy::any_executor_ref dispatch_;
    capy::any_executor_ref post_;
    waiter* waiters_ = nullptr;
    std::vector<acceptor> ports_;

    struct waiter
    {
        waiter* next;
        std::coroutine_handle<> h;
        worker_base* w;
    };

    class BOOST_COROSIO_DECL push_aw
    {
        tcp_server& self_;
        worker_base& w_;

    public:
        push_aw(tcp_server& self, worker_base& w) noexcept;
        bool await_ready() const noexcept;

        template<typename Ex>
        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<> h, Ex const&, std::stop_token) noexcept
        {
            // Dispatch to server's executor before touching shared state
            return self_.dispatch_.dispatch(h);
        }

        void await_resume() noexcept;
    };

    push_aw push(worker_base& w);

    void push_sync(worker_base& w) noexcept;

    class BOOST_COROSIO_DECL pop_aw
    {
        tcp_server& self_;
        waiter wait_;

    public:
        pop_aw(tcp_server& self) noexcept;
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
    };

    pop_aw pop();

    capy::task<void> do_accept(acceptor& acc);

protected:
    class BOOST_COROSIO_DECL worker_base
    {
        worker_base* next = nullptr;

        friend class tcp_server;
        friend class workers;

    public:
        socket sock;

        virtual ~worker_base() = default;
        virtual void run(launcher launch) = 0;

    protected:
        worker_base(capy::execution_context& ctx)
            : sock(ctx)
        {
        }
    };

    class BOOST_COROSIO_DECL workers
    {
        friend class tcp_server;

        std::vector<std::unique_ptr<worker_base>> v_;
        worker_base* idle_ = nullptr;

    public:
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
        template<class T, class... Args>
        T& emplace(Args&&... args)
        {
            auto p = std::make_unique<T>(std::forward<Args>(args)...);
            auto* raw = p.get();
            v_.push_back(std::move(p));
            push(*raw);
            return static_cast<T&>(*raw);
        }

        void reserve(std::size_t n) { v_.reserve(n); }
        std::size_t size() const noexcept { return v_.size(); }
    };

    workers wv_;

    class BOOST_COROSIO_DECL launcher
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
                -> launch_wrapper
            {
                (void)ex; // Prevent executor destruction while coroutine runs
                co_await std::move(t);
                co_await self->push(*wp);
            }(ex, srv_, std::move(task), w);

            ex.post(std::exchange(wrapper.h, nullptr)); // Release before post
            guard.w = nullptr; // Success - dismiss guard
        }
    };

protected:
    template<capy::Executor Ex>
    tcp_server(
        io_context& ctx,
        Ex const& ex)
        : ctx_(ctx)
        , dispatch_(ex)
        , post_(ex)
    {
    }

public:
    system::error_code bind(endpoint ep);

    void start();
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // corosio
} // boost

#endif
