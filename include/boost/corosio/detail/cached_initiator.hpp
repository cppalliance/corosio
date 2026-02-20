//
// Copyright (c) 2024 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_CACHED_INITIATOR_HPP
#define BOOST_COROSIO_DETAIL_CACHED_INITIATOR_HPP

#include <coroutine>
#include <cstddef>

namespace boost::corosio::detail {

/** Cached initiator coroutine frame with RAII cleanup.

    Manages the lifecycle of a cached coroutine frame used for I/O
    initiator coroutines. Automatically destroys the coroutine handle
    and frees the cached frame memory on destruction.
*/
struct cached_initiator
{
    void* frame = nullptr;
    std::coroutine_handle<> handle;

    ~cached_initiator()
    {
        if (handle)
            handle.destroy();
        if (frame)
            ::operator delete(frame);
    }

    cached_initiator()                                   = default;
    cached_initiator(cached_initiator const&)            = delete;
    cached_initiator& operator=(cached_initiator const&) = delete;

    /** Start initiator coroutine that calls Fn on impl.

        Destroys any existing coroutine, creates a new initiator that
        will call the specified member function, and returns the handle
        for symmetric transfer.

        @tparam Fn Member function pointer to call (e.g., &Impl::do_read_io)
        @param impl Pointer to the I/O object implementation
        @return Coroutine handle for symmetric transfer
    */
    template<auto Fn, class Impl>
    std::coroutine_handle<> start(Impl* impl)
    {
        if (handle)
            handle.destroy();
        auto initiator = make_initiator_coro<Fn>(frame, impl);
        handle         = initiator.h;
        return initiator.h;
    }

private:
    template<class Impl, auto Fn>
    struct initiator_coro
    {
        struct promise_type
        {
            Impl* impl;

            static void* operator new(std::size_t n, void*& cached, Impl*)
            {
                if (!cached)
                    cached = ::operator new(n);
                return cached;
            }

            static void operator delete(void*) noexcept {}

            std::suspend_always initial_suspend() noexcept
            {
                return {};
            }
            std::suspend_always final_suspend() noexcept
            {
                return {};
            }

            initiator_coro get_return_object()
            {
                return {
                    std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            void return_void() {}
            void unhandled_exception()
            {
                std::terminate();
            }
        };

        using handle_type = std::coroutine_handle<promise_type>;
        handle_type h;
    };

    template<auto Fn, class Impl>
    static initiator_coro<Impl, Fn> make_initiator_coro(void*&, Impl* impl)
    {
        (impl->*Fn)();
        co_return;
    }
};

} // namespace boost::corosio::detail

#endif
