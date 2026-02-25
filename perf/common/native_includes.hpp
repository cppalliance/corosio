//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_PERF_NATIVE_INCLUDES_HPP
#define BOOST_COROSIO_PERF_NATIVE_INCLUDES_HPP

#include <boost/corosio/native/native_tcp_socket.hpp>
#include <boost/corosio/native/native_tcp_acceptor.hpp>
#include <boost/corosio/native/native_timer.hpp>
#include <boost/corosio/native/native_io_context.hpp>

// Suite factory instantiation — returns benchmark_suite by value
#if BOOST_COROSIO_HAS_EPOLL
#define COROSIO_SUITE_INSTANTIATE_EPOLL(decl) \
    template bench::benchmark_suite decl<boost::corosio::epoll>();
#else
#define COROSIO_SUITE_INSTANTIATE_EPOLL(decl)
#endif

#if BOOST_COROSIO_HAS_KQUEUE
#define COROSIO_SUITE_INSTANTIATE_KQUEUE(decl) \
    template bench::benchmark_suite decl<boost::corosio::kqueue>();
#else
#define COROSIO_SUITE_INSTANTIATE_KQUEUE(decl)
#endif

#if BOOST_COROSIO_HAS_SELECT
#define COROSIO_SUITE_INSTANTIATE_SELECT(decl) \
    template bench::benchmark_suite decl<boost::corosio::select>();
#else
#define COROSIO_SUITE_INSTANTIATE_SELECT(decl)
#endif

#if BOOST_COROSIO_HAS_IOCP
#define COROSIO_SUITE_INSTANTIATE_IOCP(decl) \
    template bench::benchmark_suite decl<boost::corosio::iocp>();
#else
#define COROSIO_SUITE_INSTANTIATE_IOCP(decl)
#endif

#define COROSIO_SUITE_INSTANTIATE(decl)    \
    COROSIO_SUITE_INSTANTIATE_EPOLL(decl)  \
    COROSIO_SUITE_INSTANTIATE_KQUEUE(decl) \
    COROSIO_SUITE_INSTANTIATE_SELECT(decl) \
    COROSIO_SUITE_INSTANTIATE_IOCP(decl)

#endif // BOOST_COROSIO_PERF_NATIVE_INCLUDES_HPP
