//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CALLBACK_DETAIL_OP_HPP
#define CALLBACK_DETAIL_OP_HPP

#include <corosio/io_context.hpp>

#include <cstddef>
#include <utility>

namespace callback::detail {

// Thread-local cache for operation recycling
struct op_cache
{
    ~op_cache()
    {
        if(ptr_)
            ::operator delete(ptr_);
    }

    static void* allocate(std::size_t n)
    {
        auto& c = get();
        if(c.ptr_ && c.size_ >= n)
        {
            void* p = c.ptr_;
            c.ptr_ = nullptr;
            return p;
        }
        return ::operator new(n);
    }

    static void deallocate(void* p, std::size_t n)
    {
        auto& c = get();
        if(!c.ptr_ || n >= c.size_)
        {
            ::operator delete(c.ptr_);
            c.ptr_ = p;
            c.size_ = n;
        }
        else
        {
            ::operator delete(p);
        }
    }

private:
    void* ptr_ = nullptr;
    std::size_t size_ = 0;

    static op_cache& get()
    {
        static thread_local op_cache c{};
        return c;
    }
};

// Native callback operations
template<class Executor, class Handler>
struct io_op : capy::executor_work
{
    Executor ex_;
    Handler handler_;

    virtual ~io_op() = default;

    io_op(Executor ex, Handler h) : ex_(ex), handler_(std::move(h)) {}

    static void* operator new(std::size_t n) { return op_cache::allocate(n); }

    static void operator delete(void* p, std::size_t n) { op_cache::deallocate(p, n); }

    void operator()() override
    {
        auto h = std::move(handler_);
        auto ex = ex_;
        destroy();
        ex.dispatch(std::move(h));
    }

    void destroy() override { delete this; }
};

//----------------------------------------------------------

template<class Stream, class Handler>
struct read_op
{
    Stream* stream_;
    Handler handler_;
    int count_ = 0;

    read_op(Stream& stream, Handler h) : stream_(&stream), handler_(std::move(h)) {}

    void operator()()
    {
        if(count_++ < 5)
        {
            stream_->async_read_some(std::move(*this));
            return;
        }
        handler_();
    }
};

template<class Stream, class Handler>
struct request_op
{
    Stream* stream_;
    Handler handler_;
    int count_ = 0;

    request_op(Stream& stream, Handler h) : stream_(&stream), handler_(std::move(h)) {}

    void operator()();
};

template<class Stream, class Handler>
struct session_op
{
    Stream* stream_;
    Handler handler_;
    int count_ = 0;

    session_op(Stream& stream, Handler h) : stream_(&stream), handler_(std::move(h)) {}

    void operator()();
};

template<class Stream, class Handler>
struct tls_read_op
{
    Stream* stream_;
    Handler handler_;
    int count_ = 0;

    tls_read_op(Stream& stream, Handler h) : stream_(&stream), handler_(std::move(h)) {}

    void operator()()
    {
        if(count_++ < 1)
        {
            stream_->async_read_some(std::move(*this));
            return;
        }
        handler_();
    }
};

} // namespace callback::detail

#endif
