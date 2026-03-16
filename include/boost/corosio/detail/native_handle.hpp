//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_NATIVE_HANDLE_HPP
#define BOOST_COROSIO_DETAIL_NATIVE_HANDLE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>

#include <cstdint>

namespace boost::corosio {

/// Represent a platform-specific socket descriptor (`int` on POSIX, `SOCKET` on Windows).
#if BOOST_COROSIO_HAS_IOCP && !defined(BOOST_COROSIO_MRDOCS)
using native_handle_type = std::uintptr_t;
#else
using native_handle_type = int;
#endif

} // namespace boost::corosio

#endif // BOOST_COROSIO_DETAIL_NATIVE_HANDLE_HPP
