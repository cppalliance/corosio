//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_CONTEXT_HPP
#define BOOST_COROSIO_IO_CONTEXT_HPP

#include <boost/corosio/platform_reactor.hpp>
#include <boost/capy/service_provider.hpp>
#include <boost/capy/executor.hpp>

#include <concepts>
#include <utility>

namespace boost {
namespace corosio {

/** A simple I/O context for running asynchronous operations.

    The io_context provides an execution environment for async operations.
    It maintains a queue of pending work items and processes them when
    `run()` is called.

    The nested `executor` type provides the interface for dispatching
    coroutines and posting work items. It implements both synchronous
    dispatch (for symmetric transfer) and deferred posting.

    @par Example
    @code
    io_context ioc;
    auto ex = ioc.get_executor();
    async_run(ex, my_coroutine());
    ioc.run();  // Process all queued work
    @endcode

    @note This is a simplified implementation for benchmarking purposes.
    Production implementations would integrate with OS-level async I/O.

    @see executor_work
    @see executor_work_queue
    @see executor_base
    @see service_provider
*/
struct io_context : capy::service_provider
{
    explicit io_context(bool useMutex = false)
        : reactor_(useMutex
                  ? static_cast<platform_reactor*>(&make_service<platform_reactor_multi>())
                  : static_cast<platform_reactor*>(&make_service<platform_reactor_single>()))
    {}

    struct executor : capy::executor_base
    {
        io_context* ctx_;

        executor() : ctx_(nullptr) {}
        executor(io_context* ctx) : ctx_(ctx) {}

        // For coroutines: return handle for symmetric transfer
        capy::coro dispatch(capy::coro h) const override { return h; }

        // For callbacks: invoke immediately
        template<class F>
            requires(!std::same_as<std::decay_t<F>, capy::coro>)
        void dispatch(F&& f) const
        {
            std::forward<F>(f)();
        }

        void post(capy::executor_work* w) const override { ctx_->reactor_->submit(w); }

        bool operator==(executor const& other) const noexcept { return ctx_ == other.ctx_; }
    };

    executor get_executor() { return {this}; }

    void stop() {}

    void run() { reactor_->process(); }

private:
    platform_reactor* reactor_;
};

} // namespace corosio
} // namespace boost

#endif
