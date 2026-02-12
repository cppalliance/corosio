//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_SRC_DETAIL_CONDITIONAL_MUTEX_HPP
#define BOOST_COROSIO_SRC_DETAIL_CONDITIONAL_MUTEX_HPP

#include <chrono>
#include <condition_variable>
#include <mutex>

/*
    Conditional locking primitives for single-threaded optimization.

    When concurrency_hint == 1, the user guarantees single-threaded access.
    All mutex operations become no-ops, eliminating pthread_mutex overhead
    on every I/O operation.

    When locking is enabled, lock() spins briefly (spin_count_ iterations)
    before falling back to the OS mutex. This avoids the ~1-2μs futex
    round-trip for the scheduler's short critical sections.

    conditional_mutex satisfies BasicLockable, so std::lock_guard works
    via CTAD. The scheduler uses conditional_unique_lock + conditional_event
    because std::condition_variable::wait() requires std::unique_lock<std::mutex>.
*/

namespace boost::corosio::detail {

inline void spin_pause() noexcept
{
#if defined(__aarch64__) || defined(_M_ARM64)
    __asm__ volatile("yield");
#elif defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    __builtin_ia32_pause();
#endif
}

class conditional_mutex
{
public:
    explicit conditional_mutex(bool enabled = true, int spin_count = 16) noexcept
        : enabled_(enabled)
        , spin_count_(spin_count)
    {
    }

    conditional_mutex(conditional_mutex const&) = delete;
    conditional_mutex& operator=(conditional_mutex const&) = delete;

    void lock()
    {
        if (!enabled_)
            return;
        for (int i = 0; i < spin_count_; ++i)
        {
            if (mutex_.try_lock())
                return;
            spin_pause();
        }
        mutex_.lock();
    }

    void unlock()
    {
        if (enabled_)
            mutex_.unlock();
    }

    bool try_lock()
    {
        return !enabled_ || mutex_.try_lock();
    }

    void set_enabled(bool v) noexcept { enabled_ = v; }
    void set_spin_count(int n) noexcept { spin_count_ = n; }
    bool enabled() const noexcept { return enabled_; }
    int spin_count() const noexcept { return spin_count_; }
    std::mutex& underlying() noexcept { return mutex_; }

private:
    std::mutex mutex_;
    bool enabled_;
    int spin_count_;
};

class conditional_unique_lock
{
public:
    explicit conditional_unique_lock(conditional_mutex& m)
        : real_lock_(m.underlying(), std::defer_lock)
        , enabled_(m.enabled())
        , spin_count_(m.spin_count())
    {
        if (enabled_)
            spin_lock();
    }

    conditional_unique_lock(conditional_unique_lock const&) = delete;
    conditional_unique_lock& operator=(conditional_unique_lock const&) = delete;

    void lock()
    {
        if (enabled_)
            spin_lock();
    }

    void unlock()
    {
        if (enabled_)
            real_lock_.unlock();
    }

    bool owns_lock() const noexcept
    {
        return !enabled_ || real_lock_.owns_lock();
    }

    std::unique_lock<std::mutex>& underlying() noexcept { return real_lock_; }

private:
    void spin_lock()
    {
        for (int i = 0; i < spin_count_; ++i)
        {
            if (real_lock_.try_lock())
                return;
            spin_pause();
        }
        real_lock_.lock();
    }

    std::unique_lock<std::mutex> real_lock_;
    bool enabled_;
    int spin_count_;
};

class conditional_event
{
public:
    void notify_one() { cond_.notify_one(); }
    void notify_all() { cond_.notify_all(); }

    void wait(conditional_unique_lock& lock)
    {
        if (lock.underlying().owns_lock())
            cond_.wait(lock.underlying());
    }

    template<class Rep, class Period>
    void wait_for(
        conditional_unique_lock& lock,
        std::chrono::duration<Rep, Period> const& dur)
    {
        if (lock.underlying().owns_lock())
            cond_.wait_for(lock.underlying(), dur);
    }

private:
    std::condition_variable cond_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_SRC_DETAIL_CONDITIONAL_MUTEX_HPP
