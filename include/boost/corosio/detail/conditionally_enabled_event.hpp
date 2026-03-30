//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_CONDITIONALLY_ENABLED_EVENT_HPP
#define BOOST_COROSIO_DETAIL_CONDITIONALLY_ENABLED_EVENT_HPP

#include <boost/corosio/detail/conditionally_enabled_mutex.hpp>

#include <chrono>
#include <condition_variable>

namespace boost::corosio::detail {

/* Condition variable wrapper that becomes a no-op when disabled.

   When enabled, notify/wait delegate to an underlying
   std::condition_variable. When disabled, all operations
   are no-ops. The wait paths are unreachable in
   single-threaded mode because the task sentinel prevents
   the empty-queue state in do_one().
*/
class conditionally_enabled_event
{
    std::condition_variable cond_;
    bool enabled_;

public:
    explicit conditionally_enabled_event(bool enabled = true) noexcept
        : enabled_(enabled)
    {
    }

    conditionally_enabled_event(conditionally_enabled_event const&)            = delete;
    conditionally_enabled_event& operator=(conditionally_enabled_event const&) = delete;

    void set_enabled(bool v) noexcept
    {
        enabled_ = v;
    }

    void notify_one()
    {
        if (enabled_)
            cond_.notify_one();
    }

    void notify_all()
    {
        if (enabled_)
            cond_.notify_all();
    }

    void wait(conditionally_enabled_mutex::scoped_lock& lock)
    {
        if (enabled_)
            cond_.wait(lock.underlying());
    }

    template<class Rep, class Period>
    void wait_for(
        conditionally_enabled_mutex::scoped_lock& lock,
        std::chrono::duration<Rep, Period> const& d)
    {
        if (enabled_)
            cond_.wait_for(lock.underlying(), d);
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_DETAIL_CONDITIONALLY_ENABLED_EVENT_HPP
