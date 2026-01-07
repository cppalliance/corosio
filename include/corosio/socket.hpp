//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef COROSIO_SOCKET_HPP
#define COROSIO_SOCKET_HPP

#include <corosio/platform_reactor.hpp>
#include <capy/service_provider.hpp>
#include <capy/detail/frame_pool.hpp>
#include <capy/executor.hpp>

#include <cassert>
#include <coroutine>
#include <cstddef>
#include <memory>

extern std::size_t g_io_count;

namespace corosio {

/** A simulated asynchronous socket for benchmarking coroutine I/O.

    This class models an asynchronous socket that provides I/O operations
    returning awaitable types. It demonstrates the affine awaitable protocol
    where the awaitable receives the caller's executor for completion dispatch.

    The socket owns a frame allocator pool that coroutines using this socket
    can access via `get_frame_allocator()`. This enables allocation elision
    for coroutine frames when the socket is passed as a parameter.

    @note This is a simulation for benchmarking purposes. Real implementations
    would integrate with OS-level async I/O facilities.

    @see async_read_some_t
    @see has_frame_allocator
*/
struct socket
{
    struct async_read_some_t
    {
        async_read_some_t(socket& s) : s_(s) {}
        bool await_ready() const noexcept { return false; }
        void await_resume() const noexcept {}

        std::coroutine_handle<> await_suspend(capy::coro h, capy::executor_base const& ex) const
        {
            s_.do_read_some(h, ex);
            // Affine awaitable: receive caller's executor for completion dispatch.
            // Return noop because we post work rather than resuming inline.
            return std::noop_coroutine();
        }

    private:
        socket& s_;
    };

    explicit socket(capy::service_provider& sp)
        : reactor_(sp.find_service<platform_reactor>()),
          read_op_(new read_state)
    {
        assert(reactor_ != nullptr);
    }

    async_read_some_t async_read_some() { return async_read_some_t(*this); }

    capy::detail::frame_pool& get_frame_allocator() { return pool_; }

private:
    struct read_state final : capy::executor_work
    {
        capy::coro h_;
        capy::executor_base const* ex_;

        void operator()() override { ex_->dispatch(h_)(); }

        void destroy() override
        {
            // Not meant to be destroyed; owned by std::unique_ptr in socket
        }
    };

    void do_read_some(capy::coro h, capy::executor_base const& ex)
    {
        ++g_io_count;
        read_op_->h_ = h;
        read_op_->ex_ = &ex;
        reactor_->submit(read_op_.get());
    }

    platform_reactor* reactor_;
    std::unique_ptr<read_state> read_op_;
    capy::detail::frame_pool pool_;
};

} // namespace corosio

#endif

