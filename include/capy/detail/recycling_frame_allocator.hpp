//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CAPY_DETAIL_RECYCLING_FRAME_ALLOCATOR_HPP
#define CAPY_DETAIL_RECYCLING_FRAME_ALLOCATOR_HPP

#include <capy/frame_allocator.hpp>

#include <cstddef>
#include <mutex>

namespace capy::detail {

// Recycling frame allocator: thread-local with global overflow
// Tracks block sizes to avoid returning undersized blocks
class recycling_frame_allocator : public frame_allocator_base
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
    void* allocate(std::size_t n) override
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

    void deallocate(void* p, std::size_t) override
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
    static recycling_frame_allocator& shared()
    {
        static recycling_frame_allocator pool;
        return pool;
    }
};

static_assert(frame_allocator<recycling_frame_allocator>);

} // namespace capy::detail

#endif
