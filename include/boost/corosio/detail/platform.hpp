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

#ifdef BOOST_COROSIO_MRDOCS

// MrDocs documentation build: enable all backends so every
// platform-specific tag and specialization appears in the
// generated reference.  The native_* headers skip the real
// implementation includes under this guard, so no platform
// system headers are required.
#define BOOST_COROSIO_HAS_IOCP 1
#define BOOST_COROSIO_HAS_EPOLL 1
#define BOOST_COROSIO_HAS_KQUEUE 1
#define BOOST_COROSIO_HAS_SELECT 1
#define BOOST_COROSIO_POSIX 1

#else // !BOOST_COROSIO_MRDOCS

// IOCP - Windows I/O completion ports
#if defined(_WIN32)
#define BOOST_COROSIO_HAS_IOCP 1
#else
#define BOOST_COROSIO_HAS_IOCP 0
#endif

// epoll - Linux event notification
#if defined(__linux__)
#define BOOST_COROSIO_HAS_EPOLL 1
#else
#define BOOST_COROSIO_HAS_EPOLL 0
#endif

// kqueue - BSD/macOS event notification
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#define BOOST_COROSIO_HAS_KQUEUE 1
#else
#define BOOST_COROSIO_HAS_KQUEUE 0
#endif

// select - POSIX portable (available on all non-Windows)
#if !defined(_WIN32)
#define BOOST_COROSIO_HAS_SELECT 1
#else
#define BOOST_COROSIO_HAS_SELECT 0
#endif

// POSIX APIs (signals, resolver, etc.)
#if !defined(_WIN32)
#define BOOST_COROSIO_POSIX 1
#else
#define BOOST_COROSIO_POSIX 0
#endif

// fdatasync availability — Linux and FreeBSD provide it.
// macOS does not have fdatasync; sync_data() falls back to fsync().
// We use platform detection rather than _POSIX_SYNCHRONIZED_IO
// because <unistd.h> may not have been included yet.
#if defined(__linux__) || defined(__FreeBSD__)
#define BOOST_COROSIO_HAS_POSIX_SYNCHRONIZED_IO 1
#else
#define BOOST_COROSIO_HAS_POSIX_SYNCHRONIZED_IO 0
#endif

#endif // BOOST_COROSIO_MRDOCS

#endif // BOOST_COROSIO_DETAIL_PLATFORM_HPP
