//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SERVICE_STATE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SERVICE_STATE_HPP

#include <boost/corosio/detail/object_pool.hpp>
#include <boost/corosio/detail/intrusive.hpp>

namespace boost::corosio::detail {

/** Shared service state for reactor backends.

    Holds the scheduler reference and the impl recycling pool. Used
    by both socket and acceptor services.

    @tparam Scheduler The backend's scheduler type.
    @tparam Impl The backend's socket or acceptor impl type.
*/
template<class Scheduler, class Impl>
struct reactor_service_state
{
    /// Construct with a reference to the owning scheduler.
    explicit reactor_service_state(Scheduler& sched) noexcept : sched_(sched) {}

    /// Reference to the owning scheduler.
    Scheduler& sched_;

    /// Owns every impl: live for shutdown traversal, free for reuse.
    object_pool<Impl> pool_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SERVICE_STATE_HPP
