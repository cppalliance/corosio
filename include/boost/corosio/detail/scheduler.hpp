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
#include <boost/capy/executor.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstddef>

namespace boost {
namespace corosio {
namespace detail {

struct scheduler
{
    virtual ~scheduler() = default;
    virtual void post(capy::coro) const = 0;
    virtual void post(capy::executor_work*) const = 0;
    virtual void defer(capy::coro) const = 0;
    virtual void on_work_started() noexcept = 0;
    virtual void on_work_finished() noexcept = 0;
    virtual bool running_in_this_thread() const noexcept = 0;
    virtual void stop() = 0;
    virtual bool stopped() const noexcept = 0;
    virtual void restart() = 0;
    virtual std::size_t run(system::error_code& ec) = 0;
    virtual std::size_t run_one(system::error_code& ec) = 0;
    virtual std::size_t run_one(long usec, system::error_code& ec) = 0;
    virtual std::size_t wait_one(long usec, system::error_code& ec) = 0;
    virtual std::size_t run_for(std::chrono::steady_clock::duration) = 0;
    virtual std::size_t run_until(std::chrono::steady_clock::time_point) = 0;
    virtual std::size_t poll(system::error_code& ec) = 0;
    virtual std::size_t poll_one(system::error_code& ec) = 0;
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
