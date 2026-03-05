//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_SCHEDULER_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_SCHEDULER_HPP

#include <boost/corosio/detail/scheduler.hpp>

namespace boost::corosio::detail {

class timer_service;

/** Cache service pointers for native backend schedulers.

    Sits between @ref scheduler and the concrete backend schedulers,
    storing service pointers that would otherwise require a virtual
    call or service lookup on every timer operation.

    @see scheduler
*/
struct native_scheduler : scheduler
{
    /// Store the timer service pointer, set during construction.
    timer_service* timer_svc_ = nullptr;
};

} // namespace boost::corosio::detail

#endif
