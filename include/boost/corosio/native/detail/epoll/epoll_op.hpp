//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_OP_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_OP_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>

namespace boost::corosio::detail {

/// Per-descriptor state for persistent epoll registration.
struct descriptor_state final : reactor_descriptor_state
{};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_OP_HPP
