//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_SRC_DETAIL_TIMER_SERVICE_HPP
#define BOOST_COROSIO_SRC_DETAIL_TIMER_SERVICE_HPP

#include <boost/corosio/timer.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <chrono>
#include <cstddef>

namespace boost::corosio::detail {

struct scheduler;

class timer_service : public capy::execution_context::service
{
public:
    using clock_type = std::chrono::steady_clock;
    using time_point = clock_type::time_point;

    // Nested callback type - context + function pointer
    class callback
    {
        void* ctx_ = nullptr;
        void(*fn_)(void*) = nullptr;

    public:
        callback() = default;
        callback(void* ctx, void(*fn)(void*)) noexcept
            : ctx_(ctx), fn_(fn) {}

        explicit operator bool() const noexcept { return fn_ != nullptr; }
        void operator()() const { if (fn_) fn_(ctx_); }
    };

    // Create timer implementation
    virtual timer::timer_impl* create_impl() = 0;

    // Query methods for scheduler
    virtual bool empty() const noexcept = 0;
    virtual time_point nearest_expiry() const noexcept = 0;

    // Process expired timers - scheduler calls this after wait
    virtual std::size_t process_expired() = 0;

    // Callback for when earliest timer changes
    virtual void set_on_earliest_changed(callback cb) = 0;

protected:
    timer_service() = default;
};

// Get or create the timer service for the given context
timer_service&
get_timer_service(
    capy::execution_context& ctx, scheduler& sched);

} // namespace boost::corosio::detail

#endif
