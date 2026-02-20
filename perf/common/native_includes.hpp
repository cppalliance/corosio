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

// Explicit template instantiation for each available backend.
// All benchmark entry points share the same parameter signature.
#define COROSIO_BENCH_PARAMS_ \
    (perf::context_factory, bench::result_collector&, char const*, double)

#if BOOST_COROSIO_HAS_EPOLL
#define COROSIO_BENCH_INSTANTIATE_EPOLL(decl) \
    template decl<boost::corosio::epoll> COROSIO_BENCH_PARAMS_;
#else
#define COROSIO_BENCH_INSTANTIATE_EPOLL(decl)
#endif

#if BOOST_COROSIO_HAS_KQUEUE
#define COROSIO_BENCH_INSTANTIATE_KQUEUE(decl) \
    template decl<boost::corosio::kqueue> COROSIO_BENCH_PARAMS_;
#else
#define COROSIO_BENCH_INSTANTIATE_KQUEUE(decl)
#endif

#if BOOST_COROSIO_HAS_SELECT
#define COROSIO_BENCH_INSTANTIATE_SELECT(decl) \
    template decl<boost::corosio::select> COROSIO_BENCH_PARAMS_;
#else
#define COROSIO_BENCH_INSTANTIATE_SELECT(decl)
#endif

#if BOOST_COROSIO_HAS_IOCP
#define COROSIO_BENCH_INSTANTIATE_IOCP(decl) \
    template decl<boost::corosio::iocp> COROSIO_BENCH_PARAMS_;
#else
#define COROSIO_BENCH_INSTANTIATE_IOCP(decl)
#endif

#define COROSIO_BENCH_INSTANTIATE(decl)    \
    COROSIO_BENCH_INSTANTIATE_EPOLL(decl)  \
    COROSIO_BENCH_INSTANTIATE_KQUEUE(decl) \
    COROSIO_BENCH_INSTANTIATE_SELECT(decl) \
    COROSIO_BENCH_INSTANTIATE_IOCP(decl)

#endif // BOOST_COROSIO_PERF_NATIVE_INCLUDES_HPP
