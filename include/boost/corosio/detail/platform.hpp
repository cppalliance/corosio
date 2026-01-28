//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_PLATFORM_HPP
#define BOOST_COROSIO_DETAIL_PLATFORM_HPP

// Platform feature detection
// Each macro is always defined to either 0 or 1

// IOCP - Windows I/O completion ports
#if defined(_WIN32)
#  define BOOST_COROSIO_HAS_IOCP 1
#else
#  define BOOST_COROSIO_HAS_IOCP 0
#endif

// epoll - Linux event notification
#if defined(__linux__)
#  define BOOST_COROSIO_HAS_EPOLL 1
#else
#  define BOOST_COROSIO_HAS_EPOLL 0
#endif

// kqueue - BSD/macOS event notification
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#  define BOOST_COROSIO_HAS_KQUEUE 1
#else
#  define BOOST_COROSIO_HAS_KQUEUE 0
#endif

// select - POSIX portable (available on all non-Windows)
#if !defined(_WIN32)
#  define BOOST_COROSIO_HAS_SELECT 1
#else
#  define BOOST_COROSIO_HAS_SELECT 0
#endif

// POSIX APIs (signals, resolver, etc.)
#if !defined(_WIN32)
#  define BOOST_COROSIO_POSIX 1
#else
#  define BOOST_COROSIO_POSIX 0
#endif

#endif // BOOST_COROSIO_DETAIL_PLATFORM_HPP
