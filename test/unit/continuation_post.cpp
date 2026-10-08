//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/io_context.hpp>

#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>

#include "alloc_counter.hpp"
#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

namespace {

struct bare_post_awaitable
{
    capy::continuation* cont;

    bool await_ready() const noexcept
    {
        return false;
    }

    void
    await_suspend(std::coroutine_handle<> h, capy::io_env const* env) noexcept
    {
        cont->h = h;
        env->executor.post(*cont);
    }

    void await_resume() const noexcept {}
};

inline capy::task<>
bare_post_task(capy::continuation* cont, bool& ran)
{
    co_await bare_post_awaitable{cont};
    ran = true;
}

} // namespace

// Definitions for alloc_counter.hpp's extern declarations. Not in the
// anonymous namespace above: zero_alloc.cpp needs these by extern
// reference, and an unnamed namespace's members are unreachable from
// another translation unit even though formally external-linkage.
#ifndef COROSIO_TEST_HAS_TSAN
std::atomic<bool> alloc_armed{false};
std::atomic<long long> alloc_count{0};

namespace {

inline void
count_alloc() noexcept
{
    if (alloc_armed.load(std::memory_order_relaxed))
        alloc_count.fetch_add(1, std::memory_order_relaxed);
}

/* Over-aligned new/delete, implemented without a platform-specific
   aligned allocator (aligned_alloc/posix_memalign/_aligned_malloc all
   differ in availability or requirements): stash the raw malloc()
   pointer just before the returned block so the matching delete can
   recover it, the same trick a custom allocator uses.
*/
void*
aligned_alloc_impl(std::size_t n, std::size_t align)
{
    if (align < alignof(void*))
        align = alignof(void*);
    std::size_t total = (n ? n : 1) + align + sizeof(void*);
    void* raw         = std::malloc(total);
    if (!raw)
        return nullptr;
    auto base    = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*);
    auto aligned = (base + align - 1) & ~(align - 1);
    *reinterpret_cast<void**>(aligned - sizeof(void*)) = raw;
    return reinterpret_cast<void*>(aligned);
}

void
aligned_free_impl(void* p) noexcept
{
    if (!p)
        return;
    void* raw = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(p) - sizeof(void*));
    std::free(raw);
}

} // namespace
#endif

} // namespace boost::corosio

#ifndef COROSIO_TEST_HAS_TSAN
// Plain forms
void*
operator new(std::size_t n)
{
    boost::corosio::count_alloc();
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc{};
}

void*
operator new[](std::size_t n)
{
    boost::corosio::count_alloc();
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc{};
}

void
operator delete(void* p) noexcept
{
    std::free(p);
}
void
operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}
void
operator delete[](void* p) noexcept
{
    std::free(p);
}
void
operator delete[](void* p, std::size_t) noexcept
{
    std::free(p);
}

// nothrow forms
void*
operator new(std::size_t n, std::nothrow_t const&) noexcept
{
    boost::corosio::count_alloc();
    return std::malloc(n ? n : 1);
}
void*
operator new[](std::size_t n, std::nothrow_t const&) noexcept
{
    boost::corosio::count_alloc();
    return std::malloc(n ? n : 1);
}
void
operator delete(void* p, std::nothrow_t const&) noexcept
{
    std::free(p);
}
void
operator delete[](void* p, std::nothrow_t const&) noexcept
{
    std::free(p);
}

// Over-aligned forms (std::align_val_t) -- an aligned allocation on a
// measured path would otherwise bypass this interposer silently.
void*
operator new(std::size_t n, std::align_val_t align)
{
    boost::corosio::count_alloc();
    if (void* p = boost::corosio::aligned_alloc_impl(
            n, static_cast<std::size_t>(align)))
        return p;
    throw std::bad_alloc{};
}
void*
operator new[](std::size_t n, std::align_val_t align)
{
    boost::corosio::count_alloc();
    if (void* p = boost::corosio::aligned_alloc_impl(
            n, static_cast<std::size_t>(align)))
        return p;
    throw std::bad_alloc{};
}
void
operator delete(void* p, std::align_val_t) noexcept
{
    boost::corosio::aligned_free_impl(p);
}
void
operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
    boost::corosio::aligned_free_impl(p);
}
void
operator delete[](void* p, std::align_val_t) noexcept
{
    boost::corosio::aligned_free_impl(p);
}
void
operator delete[](void* p, std::size_t, std::align_val_t) noexcept
{
    boost::corosio::aligned_free_impl(p);
}

// Over-aligned nothrow forms
void*
operator new(std::size_t n, std::align_val_t align, std::nothrow_t const&)
    noexcept
{
    boost::corosio::count_alloc();
    return boost::corosio::aligned_alloc_impl(
        n, static_cast<std::size_t>(align));
}
void*
operator new[](std::size_t n, std::align_val_t align, std::nothrow_t const&)
    noexcept
{
    boost::corosio::count_alloc();
    return boost::corosio::aligned_alloc_impl(
        n, static_cast<std::size_t>(align));
}
void
operator delete(
    void* p, std::align_val_t, std::nothrow_t const&) noexcept
{
    boost::corosio::aligned_free_impl(p);
}
void
operator delete[](
    void* p, std::align_val_t, std::nothrow_t const&) noexcept
{
    boost::corosio::aligned_free_impl(p);
}
#endif

namespace boost::corosio {

template<auto Backend>
struct continuation_post_test
{
    void testBarePostIsSafe()
    {
        io_context ioc(Backend);
        auto ex  = ioc.get_executor();
        bool ran = false;

        // Allocate the continuation at the start of a fresh heap block
        // so that any read before &cont lands in the ASan redzone — the
        // worst-case layout for callers that subtract a struct offset
        // from the continuation address.
        auto cont = std::make_unique<capy::continuation>();

        capy::run_async(ex)(bare_post_task(cont.get(), ran));
        ioc.run();

        BOOST_TEST(ran);
    }

    void run()
    {
        testBarePostIsSafe();
    }
};

#ifndef COROSIO_TEST_HAS_TSAN
// Assert post(continuation&) allocates nothing on backends that enqueue
// continuations directly on the ready queue (reactor + io_uring).
template<auto Backend>
struct continuation_zero_alloc_test
{
    void testPostIsZeroAlloc()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        capy::continuation cont{};
        cont.h = std::noop_coroutine();

        alloc_count.store(0);
        alloc_armed.store(true, std::memory_order_relaxed);
        ex.post(cont);
        alloc_armed.store(false, std::memory_order_relaxed);

        BOOST_TEST(alloc_count.load() == 0LL);
        ioc.run();
    }

    void run()
    {
        if (!alloc_counter_is_live("boost.corosio.continuation_post.zero_alloc"))
            return;
        testPostIsZeroAlloc();
    }
};
#endif

COROSIO_BACKEND_TESTS(continuation_post_test, "boost.corosio.continuation_post")
#ifndef COROSIO_TEST_HAS_TSAN
COROSIO_NON_IOCP_BACKEND_TESTS(
    continuation_zero_alloc_test, "boost.corosio.continuation_post.zero_alloc")
#endif

} // namespace boost::corosio
