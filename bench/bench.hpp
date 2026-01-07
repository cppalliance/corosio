//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BENCH_HPP
#define BENCH_HPP

#include "bench_detail.hpp"
#include <corosio/io_context.hpp>
#include <corosio/platform_reactor.hpp>

#include <cstddef>
#include <utility>

extern std::size_t g_io_count;

namespace cb {

// Size of the largest possible member function pointer
constexpr std::size_t memfn_size = [] {
    struct base1
    {
        virtual void f1() {}
    };
    struct base2
    {
        virtual void f2() {}
    };
    struct derived : virtual base1, virtual base2
    {
        virtual void f3() {}
    };
    return sizeof(void (derived::*)());
}();

static_assert(memfn_size >= 2 * sizeof(void*),
    "member function pointer size should be at least 2 * sizeof(void*)");

/** A callback struct that mimics the size of member function pointers.

    This struct is designed to have the same size as a member function pointer
    on the target platform, reflecting real-world callback usage where handlers
    are often member function pointers or similar-sized callable objects.
*/
struct callback
{
    static_assert(
        memfn_size >= sizeof(int*), "member function pointer size must be at least pointer size");

    int* count_ptr_;
    std::byte padding_[memfn_size - sizeof(int*)];

    explicit callback(int& count) noexcept : count_ptr_(&count)
    {
        // Initialize padding to avoid uninitialized memory
        for(std::size_t i = 0; i < sizeof(padding_); ++i)
            padding_[i] = std::byte{0};
    }

    void operator()() const noexcept { ++(*count_ptr_); }
};

static_assert(
    sizeof(callback) == memfn_size, "callback size must match member function pointer size");

//----------------------------------------------------------

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

//----------------------------------------------------------

/** A TLS stream adapter that wraps another stream.

    This class wraps a stream and provides an async_read_some
    operation that invokes the wrapped stream's async_read_some
    once, simulating TLS record layer behavior.

    @tparam Stream The stream type to wrap.
*/
template<class Stream>
struct tls_stream
{
    Stream stream_;

    template<class... Args>
    explicit tls_stream(Args&&... args) : stream_(std::forward<Args>(args)...)
    {}

    auto get_executor() const { return stream_.get_executor(); }

    template<class Handler>
    void async_read_some(Handler&& handler)
    {
        detail::tls_read_op<Stream, std::decay_t<Handler>>(
            stream_, std::forward<Handler>(handler))();
    }
};

//----------------------------------------------------------

/** Performs a composed read operation on a stream.

    This function performs 5 sequential read_some operations on the
    stream, simulating a composed read that continues until a complete
    message or buffer has been received.

    This demonstrates a 2-level composed operation: async_read calls
    the stream's async_read_some member function 5 times.

    @param stream The stream to read from.
    @param handler The completion handler to invoke when done.
*/
template<class Stream, class Handler>
void async_read(Stream& stream, Handler&& handler)
{
    detail::read_op<Stream, std::decay_t<Handler>>(stream, std::forward<Handler>(handler))();
}

//----------------------------------------------------------

/** Performs a composed request operation on a stream.

    This function performs 10 sequential read_some operations,
    simulating a higher-level protocol operation such as reading
    an HTTP request with headers and body.

    This demonstrates a 2-level composed operation: async_request
    calls the stream's async_read_some member function 10 times.

    @param stream The stream to read from.
    @param handler The completion handler to invoke when done.
*/
template<class Stream, class Handler>
void async_request(Stream& stream, Handler&& handler)
{
    detail::request_op<Stream, std::decay_t<Handler>>(stream, std::forward<Handler>(handler))();
}

//----------------------------------------------------------

/** Performs a composed session operation on a stream.

    This function performs 100 sequential async_request operations,
    simulating a complete session that handles multiple requests
    over a persistent connection.

    This demonstrates a 3-level composed operation: async_session
    calls async_request 100 times, each of which performs 10 I/O
    operations, for a total of 1000 I/O operations.

    @param stream The stream to use for the session.
    @param handler The completion handler to invoke when done.
*/
template<class Stream, class Handler>
void async_session(Stream& stream, Handler&& handler)
{
    detail::session_op<Stream, std::decay_t<Handler>>(stream, std::forward<Handler>(handler))();
}

//----------------------------------------------------------
// Deferred definitions for detail ops that call free functions

template<class Stream, class Handler>
void detail::request_op<Stream, Handler>::operator()()
{
    if(count_++ < 10)
    {
        stream_->async_read_some(std::move(*this));
        return;
    }
    handler_();
}

template<class Stream, class Handler>
void detail::session_op<Stream, Handler>::operator()()
{
    if(count_++ < 100)
    {
        async_request(*stream_, std::move(*this));
        return;
    }
    handler_();
}

} // namespace cb

#endif
