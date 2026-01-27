//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_CONFIG_BACKEND_HPP
#define BOOST_COROSIO_DETAIL_CONFIG_BACKEND_HPP

//
// Backend selection for I/O multiplexing and signal handling.
//
// I/O Backends (platform-determined):
//   BOOST_COROSIO_BACKEND_IOCP     - Windows I/O Completion Ports
//   BOOST_COROSIO_BACKEND_EPOLL    - Linux epoll
//   BOOST_COROSIO_BACKEND_KQUEUE   - BSD/macOS kqueue (future)
//   BOOST_COROSIO_BACKEND_SELECT   - POSIX select fallback (future)
//
// Signal Backends:
//   BOOST_COROSIO_SIGNAL_WIN       - Windows (SetConsoleCtrlHandler + signal)
//   BOOST_COROSIO_SIGNAL_POSIX     - POSIX (sigaction)
//

#if defined(_WIN32)
    #define BOOST_COROSIO_BACKEND_IOCP 1
    #define BOOST_COROSIO_SIGNAL_WIN 1
#elif defined(__linux__)
    #define BOOST_COROSIO_BACKEND_EPOLL 1
    #define BOOST_COROSIO_SIGNAL_POSIX 1
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__) || defined(__APPLE__)
    #define BOOST_COROSIO_BACKEND_KQUEUE 1
    #define BOOST_COROSIO_SIGNAL_POSIX 1
#else
    #define BOOST_COROSIO_BACKEND_SELECT 1
    #define BOOST_COROSIO_SIGNAL_POSIX 1
#endif

#endif // BOOST_COROSIO_DETAIL_CONFIG_BACKEND_HPP
