//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_POSIX_SIGNALS_HPP
#define BOOST_COROSIO_DETAIL_POSIX_SIGNALS_HPP


#if !defined(_WIN32)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/signal_set.hpp>
#include <boost/capy/ex/execution_context.hpp>

/*
    POSIX Signal Service
    ====================

    This header declares the abstract signal service interface. The concrete
    implementation (posix_signals_impl) is in signals.cpp.

    This follows the timer_service pattern:
    - posix_signals is an abstract base class (no scheduler dependency)
    - posix_signals_impl is the concrete implementation
    - get_signal_service(ctx, sched) creates the service with scheduler ref

    See signals.cpp for the full implementation overview.
*/

namespace boost::corosio::detail {

struct scheduler;

//------------------------------------------------------------------------------

/** Abstract signal service for POSIX backends.

    This is the base class that defines the interface. The concrete
    implementation (posix_signals_impl) is created via get_signal_service()
    which passes the scheduler reference.
*/
class posix_signals : public capy::execution_context::service
{
public:
    /** Create a new signal set implementation. */
    virtual signal_set::signal_set_impl& create_impl() = 0;

protected:
    posix_signals() = default;
};

//------------------------------------------------------------------------------

/** Get or create the signal service for the given context.

    This function is called by the concrete scheduler during initialization
    to create the signal service with a reference to itself.

    @param ctx Reference to the owning execution_context.
    @param sched Reference to the scheduler for posting completions.
    @return Reference to the signal service.
*/
posix_signals&
get_signal_service(capy::execution_context& ctx, scheduler& sched);

} // namespace boost::corosio::detail

#endif // !defined(_WIN32)

#endif // BOOST_COROSIO_DETAIL_POSIX_SIGNALS_HPP
