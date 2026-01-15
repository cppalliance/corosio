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
#include <boost/capy/ex/any_coro.hpp>

#include <cstddef>

namespace boost {
namespace corosio {
namespace detail {

class scheduler_op;

struct scheduler
{
    virtual ~scheduler() = default;
    virtual void post(capy::any_coro) const = 0;
    virtual void post(scheduler_op*) const = 0;
    virtual void on_work_started() noexcept = 0;
    virtual void on_work_finished() noexcept = 0;
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

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
