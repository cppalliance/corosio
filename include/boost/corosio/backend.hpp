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
struct scheduler;
} // namespace detail

#if BOOST_COROSIO_HAS_EPOLL

namespace detail {

class epoll_tcp_socket;
class epoll_tcp_service;
class epoll_udp_socket;
class epoll_udp_service;
class epoll_tcp_acceptor;
class epoll_tcp_acceptor_service;
class epoll_local_stream_socket;
class epoll_local_stream_service;
class epoll_local_stream_acceptor;
class epoll_local_stream_acceptor_service;
class epoll_local_datagram_socket;
class epoll_local_datagram_service;
class epoll_scheduler;

class posix_signal;
class posix_signal_service;
class posix_resolver;
class posix_resolver_service;
class posix_stream_file;
class posix_stream_file_service;
class posix_random_access_file;
class posix_random_access_file_service;

} // namespace detail

/// Backend tag for the Linux epoll I/O multiplexer.
struct epoll_t
{
    using scheduler_type            = detail::epoll_scheduler;
    using tcp_socket_type           = detail::epoll_tcp_socket;
    using tcp_service_type          = detail::epoll_tcp_service;
    using udp_socket_type           = detail::epoll_udp_socket;
    using udp_service_type          = detail::epoll_udp_service;
    using tcp_acceptor_type         = detail::epoll_tcp_acceptor;
    using tcp_acceptor_service_type = detail::epoll_tcp_acceptor_service;

    using local_stream_socket_type           = detail::epoll_local_stream_socket;
    using local_stream_service_type          = detail::epoll_local_stream_service;
    using local_stream_acceptor_type         = detail::epoll_local_stream_acceptor;
    using local_stream_acceptor_service_type = detail::epoll_local_stream_acceptor_service;
    using local_datagram_socket_type         = detail::epoll_local_datagram_socket;
    using local_datagram_service_type        = detail::epoll_local_datagram_service;

    using signal_type           = detail::posix_signal;
    using signal_service_type   = detail::posix_signal_service;
    using resolver_type         = detail::posix_resolver;
    using resolver_service_type = detail::posix_resolver_service;

    using stream_file_type                  = detail::posix_stream_file;
    using stream_file_service_type          = detail::posix_stream_file_service;
    using random_access_file_type           = detail::posix_random_access_file;
    using random_access_file_service_type   = detail::posix_random_access_file_service;

    /// Create the scheduler and services for this backend.
    BOOST_COROSIO_DECL static detail::scheduler&
    construct(capy::execution_context&, unsigned concurrency_hint);
};

/// Tag value for selecting the epoll backend.
inline constexpr epoll_t epoll{};

#endif // BOOST_COROSIO_HAS_EPOLL

#if BOOST_COROSIO_HAS_SELECT

namespace detail {

class select_tcp_socket;
class select_tcp_service;
class select_udp_socket;
class select_udp_service;
class select_tcp_acceptor;
class select_tcp_acceptor_service;
class select_local_stream_socket;
class select_local_stream_service;
class select_local_stream_acceptor;
class select_local_stream_acceptor_service;
class select_local_datagram_socket;
class select_local_datagram_service;
class select_scheduler;

class posix_signal;
class posix_signal_service;
class posix_resolver;
class posix_resolver_service;
class posix_stream_file;
class posix_stream_file_service;
class posix_random_access_file;
class posix_random_access_file_service;

} // namespace detail

/// Backend tag for the portable select() I/O multiplexer.
struct select_t
{
    using scheduler_type            = detail::select_scheduler;
    using tcp_socket_type           = detail::select_tcp_socket;
    using tcp_service_type          = detail::select_tcp_service;
    using udp_socket_type           = detail::select_udp_socket;
    using udp_service_type          = detail::select_udp_service;
    using tcp_acceptor_type         = detail::select_tcp_acceptor;
    using tcp_acceptor_service_type = detail::select_tcp_acceptor_service;

    using local_stream_socket_type           = detail::select_local_stream_socket;
    using local_stream_service_type          = detail::select_local_stream_service;
    using local_stream_acceptor_type         = detail::select_local_stream_acceptor;
    using local_stream_acceptor_service_type = detail::select_local_stream_acceptor_service;
    using local_datagram_socket_type         = detail::select_local_datagram_socket;
    using local_datagram_service_type        = detail::select_local_datagram_service;

    using signal_type           = detail::posix_signal;
    using signal_service_type   = detail::posix_signal_service;
    using resolver_type         = detail::posix_resolver;
    using resolver_service_type = detail::posix_resolver_service;

    using stream_file_type                  = detail::posix_stream_file;
    using stream_file_service_type          = detail::posix_stream_file_service;
    using random_access_file_type           = detail::posix_random_access_file;
    using random_access_file_service_type   = detail::posix_random_access_file_service;

    /// Create the scheduler and services for this backend.
    BOOST_COROSIO_DECL static detail::scheduler&
    construct(capy::execution_context&, unsigned concurrency_hint);
};

/// Tag value for selecting the select backend.
inline constexpr select_t select{};

#endif // BOOST_COROSIO_HAS_SELECT

#if BOOST_COROSIO_HAS_KQUEUE

namespace detail {

class kqueue_tcp_socket;
class kqueue_tcp_service;
class kqueue_udp_socket;
class kqueue_udp_service;
class kqueue_tcp_acceptor;
class kqueue_tcp_acceptor_service;
class kqueue_local_stream_socket;
class kqueue_local_stream_service;
class kqueue_local_stream_acceptor;
class kqueue_local_stream_acceptor_service;
class kqueue_local_datagram_socket;
class kqueue_local_datagram_service;
class kqueue_scheduler;

class posix_signal;
class posix_signal_service;
class posix_resolver;
class posix_resolver_service;
class posix_stream_file;
class posix_stream_file_service;
class posix_random_access_file;
class posix_random_access_file_service;

} // namespace detail

/// Backend tag for the BSD kqueue I/O multiplexer.
struct kqueue_t
{
    using scheduler_type            = detail::kqueue_scheduler;
    using tcp_socket_type           = detail::kqueue_tcp_socket;
    using tcp_service_type          = detail::kqueue_tcp_service;
    using udp_socket_type           = detail::kqueue_udp_socket;
    using udp_service_type          = detail::kqueue_udp_service;
    using tcp_acceptor_type         = detail::kqueue_tcp_acceptor;
    using tcp_acceptor_service_type = detail::kqueue_tcp_acceptor_service;

    using local_stream_socket_type           = detail::kqueue_local_stream_socket;
    using local_stream_service_type          = detail::kqueue_local_stream_service;
    using local_stream_acceptor_type         = detail::kqueue_local_stream_acceptor;
    using local_stream_acceptor_service_type = detail::kqueue_local_stream_acceptor_service;
    using local_datagram_socket_type         = detail::kqueue_local_datagram_socket;
    using local_datagram_service_type        = detail::kqueue_local_datagram_service;

    using signal_type           = detail::posix_signal;
    using signal_service_type   = detail::posix_signal_service;
    using resolver_type         = detail::posix_resolver;
    using resolver_service_type = detail::posix_resolver_service;

    using stream_file_type                  = detail::posix_stream_file;
    using stream_file_service_type          = detail::posix_stream_file_service;
    using random_access_file_type           = detail::posix_random_access_file;
    using random_access_file_service_type   = detail::posix_random_access_file_service;

    /// Create the scheduler and services for this backend.
    BOOST_COROSIO_DECL static detail::scheduler&
    construct(capy::execution_context&, unsigned concurrency_hint);
};

/// Tag value for selecting the kqueue backend.
inline constexpr kqueue_t kqueue{};

#endif // BOOST_COROSIO_HAS_KQUEUE

#if BOOST_COROSIO_HAS_IO_URING

namespace detail {

class io_uring_tcp_socket;
class io_uring_tcp_service;
class io_uring_udp_socket;
class io_uring_udp_service;
class io_uring_tcp_acceptor;
class io_uring_tcp_acceptor_service;
class io_uring_local_stream_socket;
class io_uring_local_stream_service;
class io_uring_local_stream_acceptor;
class io_uring_local_stream_acceptor_service;
class io_uring_local_datagram_socket;
class io_uring_local_datagram_service;
class io_uring_scheduler;

class posix_signal;
class posix_signal_service;
class posix_resolver;
class posix_resolver_service;

} // namespace detail

/// Backend tag for the Linux io_uring proactor.
struct io_uring_t
{
    using scheduler_type            = detail::io_uring_scheduler;
    using tcp_socket_type           = detail::io_uring_tcp_socket;
    using tcp_service_type          = detail::io_uring_tcp_service;
    using udp_socket_type           = detail::io_uring_udp_socket;
    using udp_service_type          = detail::io_uring_udp_service;
    using tcp_acceptor_type         = detail::io_uring_tcp_acceptor;
    using tcp_acceptor_service_type = detail::io_uring_tcp_acceptor_service;

    using local_stream_socket_type           = detail::io_uring_local_stream_socket;
    using local_stream_service_type          = detail::io_uring_local_stream_service;
    using local_stream_acceptor_type         = detail::io_uring_local_stream_acceptor;
    using local_stream_acceptor_service_type = detail::io_uring_local_stream_acceptor_service;
    using local_datagram_socket_type         = detail::io_uring_local_datagram_socket;
    using local_datagram_service_type        = detail::io_uring_local_datagram_service;

    using signal_type           = detail::posix_signal;
    using signal_service_type   = detail::posix_signal_service;
    using resolver_type         = detail::posix_resolver;
    using resolver_service_type = detail::posix_resolver_service;

    /// Create the scheduler and services for this backend.
    BOOST_COROSIO_DECL static detail::scheduler&
    construct(capy::execution_context&, unsigned concurrency_hint);
};

/// Tag value for selecting the io_uring backend.
inline constexpr io_uring_t io_uring{};

#endif // BOOST_COROSIO_HAS_IO_URING

#if BOOST_COROSIO_HAS_IOCP

namespace detail {

class win_tcp_socket;
class win_tcp_service;
class win_tcp_acceptor;
class win_tcp_acceptor_service;
class win_scheduler;

class win_udp_socket;
class win_udp_service;

class win_local_stream_socket;
class win_local_stream_service;
class win_local_stream_acceptor;
class win_local_stream_acceptor_service;

class win_signal;
class win_signals;
class win_resolver;
class win_resolver_service;

class win_stream_file;
class win_file_service;
class win_random_access_file;
class win_random_access_file_service;

} // namespace detail

/** Backend tag for the Windows I/O Completion Ports multiplexer.

    Selects the IOCP-based reactor for all I/O services, including
    TCP, UDP, Unix domain sockets (AF_UNIX), signals, and name
    resolution.
*/
struct iocp_t
{
    using scheduler_type            = detail::win_scheduler;
    using tcp_socket_type           = detail::win_tcp_socket;
    using tcp_service_type          = detail::win_tcp_service;
    using tcp_acceptor_type         = detail::win_tcp_acceptor;
    using tcp_acceptor_service_type = detail::win_tcp_acceptor_service;
    using udp_socket_type           = detail::win_udp_socket;
    using udp_service_type          = detail::win_udp_service;

    /// @name Unix domain socket types
    /// @{
    using local_stream_socket_type           = detail::win_local_stream_socket;
    using local_stream_service_type          = detail::win_local_stream_service;
    using local_stream_acceptor_type         = detail::win_local_stream_acceptor;
    using local_stream_acceptor_service_type = detail::win_local_stream_acceptor_service;
    /// @}

    using signal_type           = detail::win_signal;
    using signal_service_type   = detail::win_signals;
    using resolver_type         = detail::win_resolver;
    using resolver_service_type = detail::win_resolver_service;

    using stream_file_type                = detail::win_stream_file;
    using stream_file_service_type        = detail::win_file_service;
    using random_access_file_type         = detail::win_random_access_file;
    using random_access_file_service_type = detail::win_random_access_file_service;

    /** Create the scheduler and services for this backend.

        @param ctx The execution context that owns the scheduler.
        @param concurrency_hint Hint for the number of threads that
            will call `run()`. Pass `1` for single-threaded mode.

        @return Reference to the newly created scheduler.
    */
    BOOST_COROSIO_DECL static detail::scheduler&
    construct(capy::execution_context&, unsigned concurrency_hint);
};

/// Tag value for selecting the IOCP backend.
inline constexpr iocp_t iocp{};

#endif // BOOST_COROSIO_HAS_IOCP

} // namespace boost::corosio

#endif // BOOST_COROSIO_BACKEND_HPP
