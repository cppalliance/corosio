//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_BACKEND_HPP
#define BOOST_COROSIO_BACKEND_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>

namespace boost::capy {
class execution_context;
} // namespace boost::capy

namespace boost::corosio {

namespace detail {
class scheduler;
} // namespace detail

#if BOOST_COROSIO_HAS_EPOLL

namespace detail {

class epoll_socket;
class epoll_socket_service;
class epoll_acceptor;
class epoll_acceptor_service;
class epoll_scheduler;

class posix_signal;
class posix_signal_service;
class posix_resolver;
class posix_resolver_service;

} // namespace detail

/// Backend tag for the Linux epoll I/O multiplexer.
struct epoll_t
{
    using scheduler_type        = detail::epoll_scheduler;
    using socket_type           = detail::epoll_socket;
    using socket_service_type   = detail::epoll_socket_service;
    using acceptor_type         = detail::epoll_acceptor;
    using acceptor_service_type = detail::epoll_acceptor_service;

    using signal_type           = detail::posix_signal;
    using signal_service_type   = detail::posix_signal_service;
    using resolver_type         = detail::posix_resolver;
    using resolver_service_type = detail::posix_resolver_service;

    /// Create the scheduler and services for this backend.
    BOOST_COROSIO_DECL static detail::scheduler&
    construct(capy::execution_context&, unsigned concurrency_hint);
};

/// Tag value for selecting the epoll backend.
inline constexpr epoll_t epoll{};

#endif // BOOST_COROSIO_HAS_EPOLL

#if BOOST_COROSIO_HAS_SELECT

namespace detail {

class select_socket;
class select_socket_service;
class select_acceptor;
class select_acceptor_service;
class select_scheduler;

class posix_signal;
class posix_signal_service;
class posix_resolver;
class posix_resolver_service;

} // namespace detail

/// Backend tag for the portable select() I/O multiplexer.
struct select_t
{
    using scheduler_type        = detail::select_scheduler;
    using socket_type           = detail::select_socket;
    using socket_service_type   = detail::select_socket_service;
    using acceptor_type         = detail::select_acceptor;
    using acceptor_service_type = detail::select_acceptor_service;

    using signal_type           = detail::posix_signal;
    using signal_service_type   = detail::posix_signal_service;
    using resolver_type         = detail::posix_resolver;
    using resolver_service_type = detail::posix_resolver_service;

    /// Create the scheduler and services for this backend.
    BOOST_COROSIO_DECL static detail::scheduler&
    construct(capy::execution_context&, unsigned concurrency_hint);
};

/// Tag value for selecting the select backend.
inline constexpr select_t select{};

#endif // BOOST_COROSIO_HAS_SELECT

#if BOOST_COROSIO_HAS_KQUEUE

namespace detail {

class kqueue_socket;
class kqueue_socket_service;
class kqueue_acceptor;
class kqueue_acceptor_service;
class kqueue_scheduler;

class posix_signal;
class posix_signal_service;
class posix_resolver;
class posix_resolver_service;

} // namespace detail

/// Backend tag for the BSD kqueue I/O multiplexer.
struct kqueue_t
{
    using scheduler_type        = detail::kqueue_scheduler;
    using socket_type           = detail::kqueue_socket;
    using socket_service_type   = detail::kqueue_socket_service;
    using acceptor_type         = detail::kqueue_acceptor;
    using acceptor_service_type = detail::kqueue_acceptor_service;

    using signal_type           = detail::posix_signal;
    using signal_service_type   = detail::posix_signal_service;
    using resolver_type         = detail::posix_resolver;
    using resolver_service_type = detail::posix_resolver_service;

    /// Create the scheduler and services for this backend.
    BOOST_COROSIO_DECL static detail::scheduler&
    construct(capy::execution_context&, unsigned concurrency_hint);
};

/// Tag value for selecting the kqueue backend.
inline constexpr kqueue_t kqueue{};

#endif // BOOST_COROSIO_HAS_KQUEUE

#if BOOST_COROSIO_HAS_IOCP

namespace detail {

class win_socket;
class win_sockets;
class win_acceptor;
class win_acceptor_service;
class win_scheduler;

class win_signal;
class win_signals;
class win_resolver;
class win_resolver_service;

} // namespace detail

/// Backend tag for the Windows I/O Completion Ports multiplexer.
struct iocp_t
{
    using scheduler_type        = detail::win_scheduler;
    using socket_type           = detail::win_socket;
    using socket_service_type   = detail::win_sockets;
    using acceptor_type         = detail::win_acceptor;
    using acceptor_service_type = detail::win_acceptor_service;

    using signal_type           = detail::win_signal;
    using signal_service_type   = detail::win_signals;
    using resolver_type         = detail::win_resolver;
    using resolver_service_type = detail::win_resolver_service;

    /// Create the scheduler and services for this backend.
    BOOST_COROSIO_DECL static detail::scheduler&
    construct(capy::execution_context&, unsigned concurrency_hint);
};

/// Tag value for selecting the IOCP backend.
inline constexpr iocp_t iocp{};

#endif // BOOST_COROSIO_HAS_IOCP

} // namespace boost::corosio

#endif // BOOST_COROSIO_BACKEND_HPP
