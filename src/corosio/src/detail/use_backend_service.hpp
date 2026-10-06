//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_SRC_DETAIL_USE_BACKEND_SERVICE_HPP
#define BOOST_COROSIO_SRC_DETAIL_USE_BACKEND_SERVICE_HPP

#include <boost/corosio/backend.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/platform.hpp>

#include <boost/capy/ex/execution_context.hpp>

// Each enabled backend's service definitions must be complete here:
// use_service<T> constructs T, and which T runs is decided at runtime.
// Only the services a translation unit actually instantiates generate
// code; completeness alone costs nothing at link time.
#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_file_service.hpp>
#include <boost/corosio/native/detail/iocp/win_local_stream_acceptor_service.hpp>
#include <boost/corosio/native/detail/iocp/win_object_handle_service.hpp>
#include <boost/corosio/native/detail/iocp/win_random_access_file_service.hpp>
#include <boost/corosio/native/detail/iocp/win_random_access_handle_service.hpp>
#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/native/detail/iocp/win_stream_handle_service.hpp>
#include <boost/corosio/native/detail/iocp/win_tcp_acceptor_service.hpp>
#include <boost/corosio/native/detail/iocp/win_udp_service.hpp>
#endif
#if BOOST_COROSIO_HAS_EPOLL
#include <boost/corosio/native/detail/epoll/epoll_types.hpp>
#endif
#if BOOST_COROSIO_HAS_KQUEUE
#include <boost/corosio/native/detail/kqueue/kqueue_types.hpp>
#endif
#if BOOST_COROSIO_HAS_SELECT
#include <boost/corosio/native/detail/select/select_types.hpp>
#endif
#if BOOST_COROSIO_POSIX
#include <boost/corosio/native/detail/posix/posix_random_access_file_service.hpp>
#include <boost/corosio/native/detail/posix/posix_stream_file_service.hpp>
#endif
#if BOOST_COROSIO_HAS_URING
#include <boost/corosio/native/detail/uring/uring_descriptor_service.hpp>
#include <boost/corosio/native/detail/uring/uring_random_access_file.hpp>
#include <boost/corosio/native/detail/uring/uring_stream_file.hpp>
#include <boost/corosio/native/detail/uring/uring_types.hpp>
#endif

namespace boost::corosio::detail {

// One trait per protocol so each public translation unit references
// only its own protocol's services. Reachability then follows the
// protocols a program names: a program that never names a protocol
// links none of that protocol's service code, for any backend.

template<class Tag>
struct tcp_service_of
{
    using type = typename Tag::tcp_service_type;
};

template<class Tag>
struct tcp_acceptor_service_of
{
    using type = typename Tag::tcp_acceptor_service_type;
};

template<class Tag>
struct udp_service_of
{
    using type = typename Tag::udp_service_type;
};

template<class Tag>
struct local_stream_service_of
{
    using type = typename Tag::local_stream_service_type;
};

template<class Tag>
struct local_stream_acceptor_service_of
{
    using type = typename Tag::local_stream_acceptor_service_type;
};

template<class Tag>
struct local_datagram_service_of
{
    using type = typename Tag::local_datagram_service_type;
};

template<class Tag>
struct descriptor_service_of
{
    using type = typename Tag::stream_descriptor_service_type;
};

template<class Tag>
struct stream_file_service_of
{
    using type = typename Tag::stream_file_service_type;
};

template<class Tag>
struct random_access_file_service_of
{
    using type = typename Tag::random_access_file_service_type;
};

template<class Tag>
struct random_access_handle_service_of
{
    using type = typename Tag::random_access_handle_service_type;
};

template<class Tag>
struct object_handle_service_of
{
    using type = typename Tag::object_handle_service_type;
};

template<class Tag>
struct stream_handle_service_of
{
    using type = typename Tag::stream_handle_service_type;
};

/** Get or create the backend-specific service registered under `Base`.

    The concrete service type comes from the backend tag's alias,
    selected by probing which scheduler the context was constructed
    with. The probe runs only on the first use of a protocol; after
    that the service is found under its abstract key directly.

    @throws std::logic_error If the context has no backend installed.
*/
template<template<class> class SvcOf, class Base>
Base&
use_backend_service(capy::execution_context& ctx)
{
    if (auto* svc = ctx.find_service<Base>())
        return *svc;
#if BOOST_COROSIO_HAS_IOCP
    if (ctx.find_service<win_scheduler>())
        return ctx.use_service<typename SvcOf<iocp_t>::type>();
#endif
#if BOOST_COROSIO_HAS_EPOLL
    if (ctx.find_service<epoll_scheduler>())
        return ctx.use_service<typename SvcOf<epoll_t>::type>();
#endif
#if BOOST_COROSIO_HAS_URING
    if (ctx.find_service<uring_scheduler>())
        return ctx.use_service<typename SvcOf<uring_t>::type>();
#endif
#if BOOST_COROSIO_HAS_KQUEUE
    if (ctx.find_service<kqueue_scheduler>())
        return ctx.use_service<typename SvcOf<kqueue_t>::type>();
#endif
#if BOOST_COROSIO_HAS_SELECT
    if (ctx.find_service<select_scheduler>())
        return ctx.use_service<typename SvcOf<select_t>::type>();
#endif
    throw_logic_error("service not installed");
}

} // namespace boost::corosio::detail

#endif
