//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CAPY_DETAIL_FRAME_POOL_HPP
#define CAPY_DETAIL_FRAME_POOL_HPP

#include <capy/frame_allocator.hpp>

#include <cstddef>
#include <mutex>

namespace capy::detail {

// Frame pool: thread-local with global overflow
// Tracks block sizes to avoid returning undersized blocks
class frame_pool
{
    struct block
    {
        block* next;
        std::size_t size;
    };

    struct global_pool
    {
        std::mutex mtx;
        block* head = nullptr;

        ~global_pool()
        {
            while(head)
            {
                auto p = head;
                head = head->next;
                ::operator delete(p);
            }
        }

        void push(block* b)
        {
            std::lock_guard<std::mutex> lock(mtx);
            b->next = head;
            head = b;
        }

        block* pop(std::size_t n)
        {
            std::lock_guard<std::mutex> lock(mtx);
            block** pp = &head;
            while(*pp)
            {
                // block->size stores total allocated size (including header)
                if((*pp)->size >= n + sizeof(block))
                {
                    block* p = *pp;
                    *pp = p->next;
                    return p;
                }
                pp = &(*pp)->next;
            }
            return nullptr;
        }
    };

    struct local_pool
    {
        block* head = nullptr;

        void push(block* b)
        {
            b->next = head;
            head = b;
        }

        block* pop(std::size_t n)
        {
            block** pp = &head;
            while(*pp)
            {
                // block->size stores total allocated size (including header)
                if((*pp)->size >= n + sizeof(block))
                {
                    block* p = *pp;
                    *pp = p->next;
                    return p;
                }
                pp = &(*pp)->next;
            }
            return nullptr;
        }
    };

    static local_pool& local()
    {
        static thread_local local_pool local;
        return local;
    }

public:
    void* allocate(std::size_t n)
    {
        std::size_t total = n + sizeof(block);

        if(auto* b = local().pop(n))
            return static_cast<char*>(static_cast<void*>(b)) + sizeof(block);

        if(auto* b = global().pop(n))
            return static_cast<char*>(static_cast<void*>(b)) + sizeof(block);

        auto* b = static_cast<block*>(::operator new(total));
        b->next = nullptr;
        b->size = total;
        return static_cast<char*>(static_cast<void*>(b)) + sizeof(block);
    }

    void deallocate(void* p, std::size_t)
    {
        auto* b = static_cast<block*>(static_cast<void*>(static_cast<char*>(p) - sizeof(block)));
        b->next = nullptr;
        local().push(b);
    }

    static global_pool& global()
    {
        static global_pool pool;
        return pool;
    }

    // Shared pool instance for all coroutine frames
    static frame_pool& shared()
    {
        static frame_pool pool;
        return pool;
    }

    // Mixin for promise types to provide frame allocation
    struct promise_allocator
    {
        struct header
        {
            void (*dealloc)(void* ctx, void* ptr, std::size_t size);
            void* ctx;
        };

        template<class Allocator>
        static void* allocate_with(std::size_t size, Allocator& alloc)
        {
            std::size_t total = size + sizeof(header);
            void* raw = alloc.allocate(total);

            auto* p = static_cast<header*>(raw);
            p->dealloc = [](void* ctx, void* p, std::size_t n) {
                static_cast<Allocator*>(ctx)->deallocate(p, n);
            };
            p->ctx = &alloc;

            return p + 1;
        }

        static void* operator new(std::size_t size)
        {
            return allocate_with(size, frame_pool::shared());
        }

        template<capy::has_frame_allocator Arg0, class... ArgN>
        static void* operator new(std::size_t size, Arg0& arg0, ArgN&...)
        {
            return allocate_with(size, arg0.get_frame_allocator());
        }

        template<class Arg0, capy::has_frame_allocator Arg1, class... ArgN>
        static void* operator new(std::size_t size, Arg0&, Arg1& arg1, ArgN&...)
            requires(!capy::has_frame_allocator<Arg0>)
        {
            return allocate_with(size, arg1.get_frame_allocator());
        }

        static void operator delete(void* ptr, std::size_t size)
        {
            auto* p = static_cast<header*>(ptr) - 1;
            std::size_t total = size + sizeof(header);
            p->dealloc(p->ctx, p, total);
        }
    };
};

} // namespace capy::detail

#endif
