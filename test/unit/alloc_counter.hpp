//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_ALLOC_COUNTER_HPP
#define BOOST_COROSIO_TEST_ALLOC_COUNTER_HPP

/* Shared global operator new/delete counting interposer.

   The replaceable global allocation functions are process-wide and
   may be defined exactly once; continuation_post.cpp owns that one
   definition (guarded against TSan, which replaces them itself).
   Every other test that needs to assert an allocation count --
   zero_alloc.cpp included -- counts against these same extern
   counters instead of layering on a second interposer, which would
   be a link error.
*/

#include <atomic>
#include <cstdio>
#include <new>

// TSan's runtime already replaces global operator new/delete, so a
// counting replacement cannot link under -fsanitize=thread (and the
// counts would reflect the sanitizer's allocator anyway).
#if defined(__SANITIZE_THREAD__)
#define COROSIO_TEST_HAS_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define COROSIO_TEST_HAS_TSAN 1
#endif
#endif

namespace boost::corosio {

#ifndef COROSIO_TEST_HAS_TSAN
/// True while allocations should be tallied into alloc_count.
extern std::atomic<bool> alloc_armed;
/// Running count of operator new calls while alloc_armed is set.
extern std::atomic<long long> alloc_count;

/** Check that the counting interposer actually runs, loud-skipping if not.

    ASan and TSan are excluded at compile time, but Valgrind replaces
    the global allocator at run time: the interposer never executes,
    every count reads zero, and a gate would either fail spuriously
    (a documented non-zero pin) or pass vacuously (a zero pin). The
    probe tests the behavior rather than the tool, as the fault
    harness's allocation probe does, so any runtime replacement is
    caught; the skip names its reason on stderr, as the fault suites'
    Valgrind skip does, so the leg passes with a visible cause.

    @param suite Name printed in the skip notice.

    @return `true` if allocations are observable and the gate may run.
*/
inline bool
alloc_counter_is_live(char const* suite) noexcept
{
    // Called through a volatile pointer so the probe allocation
    // cannot be elided.
    void* (*volatile op_new)(std::size_t) = &::operator new;
    alloc_count.store(0, std::memory_order_relaxed);
    alloc_armed.store(true, std::memory_order_relaxed);
    ::operator delete(op_new(1));
    alloc_armed.store(false, std::memory_order_relaxed);
    if (alloc_count.load(std::memory_order_relaxed) != 0)
        return true;
    std::fprintf(
        stderr,
        "%s: the global allocator is replaced at run time (Valgrind?), "
        "so allocation counts are unobservable; skipping this suite\n",
        suite);
    return false;
}
#endif

} // namespace boost::corosio

#endif
