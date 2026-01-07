//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CALLBACK_SOCKET_HPP
#define CALLBACK_SOCKET_HPP

#include "detail/op.hpp"

#include <corosio/platform_reactor.hpp>
#include <corosio/io_context.hpp>

extern std::size_t g_io_count;

namespace callback {

/** A simulated asynchronous socket for benchmarking callback-based I/O.

    This class models an asynchronous socket that provides I/O operations
    accepting completion handlers. It demonstrates the traditional callback
    pattern where async operations accept a handler that is invoked upon
    completion.

    The socket stores an executor which is used to post I/O completion
    work items and to dispatch completion handlers.

    @tparam Executor The executor type used for completion dispatch.

    @note This is a simulation for benchmarking purposes. Real implementations
    would integrate with OS-level async I/O facilities.
*/
struct socket
{
    corosio::io_context& ioc_;
    corosio::platform_reactor* reactor_;

    explicit socket(corosio::io_context& ioc)
        : ioc_(ioc),
          reactor_(ioc.find_service<corosio::platform_reactor>())
    {}

    auto get_executor() const { return ioc_.get_executor(); }

    template<class Handler>
    void async_read_some(Handler&& handler)
    {
        ++g_io_count;
        using op_t = detail::io_op<corosio::io_context::executor, std::decay_t<Handler>>;
        auto ex = ioc_.get_executor();
        reactor_->submit(new op_t(ex, std::forward<Handler>(handler)));
    }
};


} // namespace callback

#endif

