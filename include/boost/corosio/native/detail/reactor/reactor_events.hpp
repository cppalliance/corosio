//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_EVENTS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_EVENTS_HPP

#include <cstdint>

namespace boost::corosio::detail {

/// Shared reactor event constants.
/// These match epoll numeric values; kqueue maps its events to the same.
static constexpr std::uint32_t reactor_event_read  = 0x001;
static constexpr std::uint32_t reactor_event_write = 0x004;
static constexpr std::uint32_t reactor_event_error = 0x008;

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_EVENTS_HPP
