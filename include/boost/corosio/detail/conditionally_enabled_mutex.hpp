//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_CONDITIONALLY_ENABLED_MUTEX_HPP
#define BOOST_COROSIO_DETAIL_CONDITIONALLY_ENABLED_MUTEX_HPP

#include <mutex>

namespace boost::corosio::detail {

/* Mutex wrapper that becomes a no-op when disabled.

   When enabled (the default), lock/unlock delegate to an
   underlying std::mutex. When disabled, all operations are
   no-ops. The enabled flag is fixed after construction.

   scoped_lock wraps std::unique_lock<std::mutex> internally
   so that condvar wait paths (which require the real lock
   type) compile and work in multi-threaded mode.
*/
class conditionally_enabled_mutex
{
    std::mutex mutex_;
    bool enabled_;

public:
    explicit conditionally_enabled_mutex(bool enabled = true) noexcept
        : enabled_(enabled)
    {
    }

    conditionally_enabled_mutex(conditionally_enabled_mutex const&)            = delete;
    conditionally_enabled_mutex& operator=(conditionally_enabled_mutex const&) = delete;

    bool enabled() const noexcept
    {
        return enabled_;
    }

    void set_enabled(bool v) noexcept
    {
        enabled_ = v;
    }

    // Lockable interface — allows std::lock_guard<conditionally_enabled_mutex>
    void lock() { if (enabled_) mutex_.lock(); }
    void unlock() { if (enabled_) mutex_.unlock(); }
    bool try_lock() { return !enabled_ || mutex_.try_lock(); }

    class scoped_lock
    {
        std::unique_lock<std::mutex> lock_;
        bool enabled_;

    public:
        explicit scoped_lock(conditionally_enabled_mutex& m)
            : lock_(m.mutex_, std::defer_lock)
            , enabled_(m.enabled_)
        {
            if (enabled_)
                lock_.lock();
        }

        scoped_lock(scoped_lock const&)            = delete;
        scoped_lock& operator=(scoped_lock const&) = delete;

        void lock()
        {
            if (enabled_)
                lock_.lock();
        }

        void unlock()
        {
            if (enabled_)
                lock_.unlock();
        }

        bool owns_lock() const noexcept
        {
            return enabled_ && lock_.owns_lock();
        }

        // Access the underlying unique_lock for condvar wait().
        // Only called when locking is enabled.
        std::unique_lock<std::mutex>& underlying() noexcept
        {
            return lock_;
        }
    };
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_DETAIL_CONDITIONALLY_ENABLED_MUTEX_HPP
