//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_CONTEXT_HPP
#define BOOST_COROSIO_TEST_CONTEXT_HPP

/* Backend context includes and test registration macro.

   Include this header in test files that are templated on the context
   type. The COROSIO_BACKEND_TESTS macro generates a struct + TEST_SUITE
   registration for every backend available on the current platform.

   Test names use dot-separated backend suffixes so that the test runner's
   prefix matching works correctly:
     boost.corosio.timer          -> runs all backends
     boost.corosio.timer.kqueue   -> runs only kqueue
     boost.corosio.timer.select   -> runs only select
*/

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/io_context.hpp>

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/iocp_context.hpp>
#endif

#if BOOST_COROSIO_HAS_EPOLL
#include <boost/corosio/epoll_context.hpp>
#endif

#if BOOST_COROSIO_HAS_KQUEUE
#include <boost/corosio/kqueue_context.hpp>
#endif

#if BOOST_COROSIO_HAS_SELECT
#include <boost/corosio/select_context.hpp>
#endif

// Per-backend registration macros (empty when backend not available)

#if BOOST_COROSIO_HAS_IOCP
#define COROSIO_TEST_IOCP_(impl, name)      \
    struct impl##_iocp : impl<iocp_context> \
    {};                                     \
    TEST_SUITE(impl##_iocp, name ".iocp");
#else
#define COROSIO_TEST_IOCP_(impl, name)
#endif

#if BOOST_COROSIO_HAS_EPOLL
#define COROSIO_TEST_EPOLL_(impl, name)       \
    struct impl##_epoll : impl<epoll_context> \
    {};                                       \
    TEST_SUITE(impl##_epoll, name ".epoll");
#else
#define COROSIO_TEST_EPOLL_(impl, name)
#endif

#if BOOST_COROSIO_HAS_KQUEUE
#define COROSIO_TEST_KQUEUE_(impl, name)        \
    struct impl##_kqueue : impl<kqueue_context> \
    {};                                         \
    TEST_SUITE(impl##_kqueue, name ".kqueue");
#else
#define COROSIO_TEST_KQUEUE_(impl, name)
#endif

#if BOOST_COROSIO_HAS_SELECT
#define COROSIO_TEST_SELECT_(impl, name)        \
    struct impl##_select : impl<select_context> \
    {};                                         \
    TEST_SUITE(impl##_select, name ".select");
#else
#define COROSIO_TEST_SELECT_(impl, name)
#endif

#define COROSIO_BACKEND_TESTS(impl, name) \
    COROSIO_TEST_IOCP_(impl, name)        \
    COROSIO_TEST_EPOLL_(impl, name)       \
    COROSIO_TEST_KQUEUE_(impl, name)      \
    COROSIO_TEST_SELECT_(impl, name)

#endif
