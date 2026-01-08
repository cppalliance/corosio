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
#include <capy/affine.hpp>
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
        async_read_some_t(
            socket& s)
            : s_(s)
        {
        }

        bool await_ready() const noexcept
        {
            return false;
        }

        void await_resume() const noexcept
        {
        }

        template<capy::dispatcher Dispatcher>
        auto
        await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d) const ->
                std::coroutine_handle<>
        {
            s_.do_read_some(h, d);
            return std::noop_coroutine();
        }

    private:
        socket& s_;
    };

    explicit socket(capy::service_provider& sp);

    async_read_some_t
    async_read_some()
    {
        return async_read_some_t(*this);
    }

    capy::detail::frame_pool& get_frame_allocator()
    {
        return pool_;
    }

private:
    void do_read_some(
        std::coroutine_handle<>,
        capy::any_dispatcher);

    struct ops_state;

    platform_reactor* reactor_;
    capy::detail::frame_pool pool_;
    std::unique_ptr<ops_state, void(*)(ops_state*)> ops_;
};

} // corosio

#endif

