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
#include <cstdlib>
#include <memory>
#include <new>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

namespace {

struct bare_post_awaitable
{
    capy::continuation* cont;

    bool await_ready() const noexcept { return false; }

    void await_suspend(
        std::coroutine_handle<> h,
        capy::io_env const* env) noexcept
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

// TSan's runtime already replaces global operator new/delete, so the
// counting replacements below cannot link under -fsanitize=thread (and
// the counts would reflect the sanitizer's allocator anyway).
#if defined(__SANITIZE_THREAD__)
# define COROSIO_TEST_HAS_TSAN 1
#elif defined(__has_feature)
# if __has_feature(thread_sanitizer)
#  define COROSIO_TEST_HAS_TSAN 1
# endif
#endif

#ifndef COROSIO_TEST_HAS_TSAN
std::atomic<bool>      alloc_armed{false};
std::atomic<long long> alloc_count{0};
#endif

} // namespace

} // namespace boost::corosio

#ifndef COROSIO_TEST_HAS_TSAN
void* operator new(std::size_t n)
{
    if (boost::corosio::alloc_armed.load(std::memory_order_relaxed))
        boost::corosio::alloc_count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc{};
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace boost::corosio {

template<auto Backend>
struct continuation_post_test
{
    void testBarePostIsSafe()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
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
        testPostIsZeroAlloc();
    }
};
#endif

COROSIO_BACKEND_TESTS(continuation_post_test, "boost.corosio.continuation_post")
#ifndef COROSIO_TEST_HAS_TSAN
COROSIO_NON_IOCP_BACKEND_TESTS(
    continuation_zero_alloc_test,
    "boost.corosio.continuation_post.zero_alloc")
#endif

} // namespace boost::corosio
