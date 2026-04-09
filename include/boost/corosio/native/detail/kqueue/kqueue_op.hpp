//
// Copyright (c) 2026 Michael Vandeberg
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_OP_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_OP_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>

namespace boost::corosio::detail {

// Aliases for shared reactor event constants.
static constexpr std::uint32_t kqueue_event_read  = reactor_event_read;
static constexpr std::uint32_t kqueue_event_write = reactor_event_write;
static constexpr std::uint32_t kqueue_event_error = reactor_event_error;

/// Per-descriptor state for persistent kqueue registration.
struct descriptor_state final : reactor_descriptor_state
{};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_OP_HPP
