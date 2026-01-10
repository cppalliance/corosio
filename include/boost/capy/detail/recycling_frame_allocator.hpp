//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_CAPY_DETAIL_RECYCLING_FRAME_ALLOCATOR_HPP
#define BOOST_CAPY_DETAIL_RECYCLING_FRAME_ALLOCATOR_HPP

#include <boost/capy/frame_allocator.hpp>

#include <cstddef>
#include <mutex>

namespace boost {
namespace capy {
namespace detail {

/** Recycling frame allocator with thread-local and global pools.

    This allocator recycles memory blocks to reduce allocation overhead.
    It maintains a thread-local pool for fast lock-free access and a
    global pool for cross-thread block sharing.

    Blocks are tracked by size to avoid returning undersized blocks.

    This type satisfies the frame_allocator concept and is cheaply
    copyable (all instances share the same static pools).
*/
class recycling_frame_allocator
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

    static global_pool& global()
    {
        static global_pool pool;
        return pool;
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
};

static_assert(frame_allocator<recycling_frame_allocator>);

} // namespace detail
} // namespace capy
} // namespace boost

#endif
