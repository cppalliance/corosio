//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_SCHEDULER_HPP
#define BOOST_COROSIO_DETAIL_SCHEDULER_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/coro.hpp>

#include <cstddef>

namespace boost::corosio::detail {

class scheduler_op;

struct scheduler
{
    virtual ~scheduler() = default;
    virtual void post(capy::coro) const = 0;
    virtual void post(scheduler_op*) const = 0;

    /** Notify scheduler of pending work (for executor use).
        When the count reaches zero, the scheduler stops.
    */
    virtual void on_work_started() noexcept = 0;
    virtual void on_work_finished() noexcept = 0;

    /** Notify scheduler of pending I/O work (for services use).
        Unlike on_work_finished, work_finished does not stop the scheduler
        when the count reaches zero - it only wakes blocked threads.
    */
    virtual void work_started() const noexcept = 0;
    virtual void work_finished() const noexcept = 0;

    virtual bool running_in_this_thread() const noexcept = 0;
    virtual void stop() = 0;
    virtual bool stopped() const noexcept = 0;
    virtual void restart() = 0;
    virtual std::size_t run() = 0;
    virtual std::size_t run_one() = 0;
    virtual std::size_t wait_one(long usec) = 0;
    virtual std::size_t poll() = 0;
    virtual std::size_t poll_one() = 0;
};

} // namespace boost::corosio::detail

#endif
