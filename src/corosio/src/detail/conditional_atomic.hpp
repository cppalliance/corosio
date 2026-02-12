//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_SRC_DETAIL_CONDITIONAL_ATOMIC_HPP
#define BOOST_COROSIO_SRC_DETAIL_CONDITIONAL_ATOMIC_HPP

#include <atomic>

/*
    Conditional atomic for single-threaded optimization.

    Keeps std::atomic<T> as storage but when disabled (concurrency_hint == 1),
    decomposes RMW ops (fetch_add, fetch_sub, exchange) into relaxed
    load + modify + relaxed store — no exclusive pairs, no barriers.
    On ARM64 this turns ldaxr/stlxr (~10-20 cycles) into plain
    ldr/add/str (~3 cycles). On x86 (TSO) it eliminates LOCK prefixes.

    load/store downgrade their memory ordering to relaxed when disabled.
*/

namespace boost::corosio::detail {

template<typename T>
class conditional_atomic
{
public:
    explicit conditional_atomic(T initial = T{}, bool enabled = true) noexcept
        : atomic_(initial), enabled_(enabled) {}

    conditional_atomic(conditional_atomic const&) = delete;
    conditional_atomic& operator=(conditional_atomic const&) = delete;

    T load(std::memory_order order) const noexcept
    {
        return atomic_.load(enabled_ ? order : std::memory_order_relaxed);
    }

    void store(T val, std::memory_order order) noexcept
    {
        atomic_.store(val, enabled_ ? order : std::memory_order_relaxed);
    }

    T fetch_add(T arg, std::memory_order order) noexcept
    {
        if (enabled_)
            return atomic_.fetch_add(arg, order);
        T old = atomic_.load(std::memory_order_relaxed);
        atomic_.store(old + arg, std::memory_order_relaxed);
        return old;
    }

    T fetch_sub(T arg, std::memory_order order) noexcept
    {
        if (enabled_)
            return atomic_.fetch_sub(arg, order);
        T old = atomic_.load(std::memory_order_relaxed);
        atomic_.store(old - arg, std::memory_order_relaxed);
        return old;
    }

    T exchange(T val, std::memory_order order) noexcept
    {
        if (enabled_)
            return atomic_.exchange(val, order);
        T old = atomic_.load(std::memory_order_relaxed);
        atomic_.store(val, std::memory_order_relaxed);
        return old;
    }

    bool compare_exchange_strong(
        T& expected, T desired,
        std::memory_order success, std::memory_order failure) noexcept
    {
        if (enabled_)
            return atomic_.compare_exchange_strong(expected, desired, success, failure);
        T current = atomic_.load(std::memory_order_relaxed);
        if (current == expected)
        {
            atomic_.store(desired, std::memory_order_relaxed);
            return true;
        }
        expected = current;
        return false;
    }

    void set_enabled(bool v) noexcept { enabled_ = v; }
    bool enabled() const noexcept { return enabled_; }

private:
    std::atomic<T> atomic_;
    bool enabled_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_SRC_DETAIL_CONDITIONAL_ATOMIC_HPP
