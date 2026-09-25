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

/// Selects the Linux epoll I/O multiplexer as the backend.
struct epoll_t
{
    /// The scheduler that drives the event loop.
    using scheduler_type = detail::epoll_scheduler;
    /// The concrete TCP socket type.
    using tcp_socket_type = detail::epoll_tcp_socket;
    /// The service that owns the TCP socket implementations.
    using tcp_service_type = detail::epoll_tcp_service;
    /// The concrete UDP socket type.
    using udp_socket_type = detail::epoll_udp_socket;
    /// The service that owns the UDP socket implementations.
    using udp_service_type = detail::epoll_udp_service;
    /// The concrete TCP acceptor type.
    using tcp_acceptor_type = detail::epoll_tcp_acceptor;
    /// The service that owns the TCP acceptor implementations.
    using tcp_acceptor_service_type = detail::epoll_tcp_acceptor_service;

    /// The concrete Unix domain stream socket type.
    using local_stream_socket_type = detail::epoll_local_stream_socket;
    /// The service that owns the Unix domain stream implementations.
    using local_stream_service_type = detail::epoll_local_stream_service;
    /// The concrete Unix domain stream acceptor type.
    using local_stream_acceptor_type = detail::epoll_local_stream_acceptor;
    /// The service that owns the Unix domain acceptor implementations.
    using local_stream_acceptor_service_type =
        detail::epoll_local_stream_acceptor_service;
    /// The concrete Unix domain datagram socket type.
    using local_datagram_socket_type = detail::epoll_local_datagram_socket;
    /// The service that owns the Unix domain datagram implementations.
    using local_datagram_service_type = detail::epoll_local_datagram_service;

    /// The concrete signal set type.
    using signal_type = detail::posix_signal;
    /// The service that owns the signal set implementations.
    using signal_service_type = detail::posix_signal_service;
    /// The concrete name resolver type.
    using resolver_type = detail::posix_resolver;
    /// The service that owns the resolver implementations.
    using resolver_service_type = detail::posix_resolver_service;

    /// The concrete sequential file type.
    using stream_file_type = detail::posix_stream_file;
    /// The service that owns the sequential file implementations.
    using stream_file_service_type = detail::posix_stream_file_service;
    /// The concrete random-access file type.
    using random_access_file_type = detail::posix_random_access_file;
    /// The service that owns the random-access file implementations.
    using random_access_file_service_type =
        detail::posix_random_access_file_service;

    /** Create the scheduler and services for this backend.

        @param ctx The execution context that owns the scheduler.
        @param concurrency_hint Hint for the number of threads that
            call `run()`. A performance tuning knob; the
            thread-safety contract is set separately by
            @ref io_context_options::locking.

        @return Reference to the newly created scheduler.

        @throws std::system_error If the backend's infrastructure
            could not be created.
    */
    BOOST_COROSIO_DECL static detail::scheduler&
    construct(capy::execution_context& ctx, unsigned concurrency_hint);
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

/// Selects the portable select() I/O multiplexer as the backend.
struct select_t
{
    /// The scheduler that drives the event loop.
    using scheduler_type = detail::select_scheduler;
    /// The concrete TCP socket type.
    using tcp_socket_type = detail::select_tcp_socket;
    /// The service that owns the TCP socket implementations.
    using tcp_service_type = detail::select_tcp_service;
    /// The concrete UDP socket type.
    using udp_socket_type = detail::select_udp_socket;
    /// The service that owns the UDP socket implementations.
    using udp_service_type = detail::select_udp_service;
    /// The concrete TCP acceptor type.
    using tcp_acceptor_type = detail::select_tcp_acceptor;
    /// The service that owns the TCP acceptor implementations.
    using tcp_acceptor_service_type = detail::select_tcp_acceptor_service;

    /// The concrete Unix domain stream socket type.
    using local_stream_socket_type = detail::select_local_stream_socket;
    /// The service that owns the Unix domain stream implementations.
    using local_stream_service_type = detail::select_local_stream_service;
    /// The concrete Unix domain stream acceptor type.
    using local_stream_acceptor_type = detail::select_local_stream_acceptor;
    /// The service that owns the Unix domain acceptor implementations.
    using local_stream_acceptor_service_type =
        detail::select_local_stream_acceptor_service;
    /// The concrete Unix domain datagram socket type.
    using local_datagram_socket_type = detail::select_local_datagram_socket;
    /// The service that owns the Unix domain datagram implementations.
    using local_datagram_service_type = detail::select_local_datagram_service;

    /// The concrete signal set type.
    using signal_type = detail::posix_signal;
    /// The service that owns the signal set implementations.
    using signal_service_type = detail::posix_signal_service;
    /// The concrete name resolver type.
    using resolver_type = detail::posix_resolver;
    /// The service that owns the resolver implementations.
    using resolver_service_type = detail::posix_resolver_service;

    /// The concrete sequential file type.
    using stream_file_type = detail::posix_stream_file;
    /// The service that owns the sequential file implementations.
    using stream_file_service_type = detail::posix_stream_file_service;
    /// The concrete random-access file type.
    using random_access_file_type = detail::posix_random_access_file;
    /// The service that owns the random-access file implementations.
    using random_access_file_service_type =
        detail::posix_random_access_file_service;

    /** Create the scheduler and services for this backend.

        @param ctx The execution context that owns the scheduler.
        @param concurrency_hint Hint for the number of threads that
            call `run()`. A performance tuning knob; the
            thread-safety contract is set separately by
            @ref io_context_options::locking.

        @return Reference to the newly created scheduler.

        @throws std::system_error If the backend's infrastructure
            could not be created.
    */
    BOOST_COROSIO_DECL static detail::scheduler&
    construct(capy::execution_context& ctx, unsigned concurrency_hint);
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

/// Selects the BSD kqueue I/O multiplexer as the backend.
struct kqueue_t
{
    /// The scheduler that drives the event loop.
    using scheduler_type = detail::kqueue_scheduler;
    /// The concrete TCP socket type.
    using tcp_socket_type = detail::kqueue_tcp_socket;
    /// The service that owns the TCP socket implementations.
    using tcp_service_type = detail::kqueue_tcp_service;
    /// The concrete UDP socket type.
    using udp_socket_type = detail::kqueue_udp_socket;
    /// The service that owns the UDP socket implementations.
    using udp_service_type = detail::kqueue_udp_service;
    /// The concrete TCP acceptor type.
    using tcp_acceptor_type = detail::kqueue_tcp_acceptor;
    /// The service that owns the TCP acceptor implementations.
    using tcp_acceptor_service_type = detail::kqueue_tcp_acceptor_service;

    /// The concrete Unix domain stream socket type.
    using local_stream_socket_type = detail::kqueue_local_stream_socket;
    /// The service that owns the Unix domain stream implementations.
    using local_stream_service_type = detail::kqueue_local_stream_service;
    /// The concrete Unix domain stream acceptor type.
    using local_stream_acceptor_type = detail::kqueue_local_stream_acceptor;
    /// The service that owns the Unix domain acceptor implementations.
    using local_stream_acceptor_service_type =
        detail::kqueue_local_stream_acceptor_service;
    /// The concrete Unix domain datagram socket type.
    using local_datagram_socket_type = detail::kqueue_local_datagram_socket;
    /// The service that owns the Unix domain datagram implementations.
    using local_datagram_service_type = detail::kqueue_local_datagram_service;

    /// The concrete signal set type.
    using signal_type = detail::posix_signal;
    /// The service that owns the signal set implementations.
    using signal_service_type = detail::posix_signal_service;
    /// The concrete name resolver type.
    using resolver_type = detail::posix_resolver;
    /// The service that owns the resolver implementations.
    using resolver_service_type = detail::posix_resolver_service;

    /// The concrete sequential file type.
    using stream_file_type = detail::posix_stream_file;
    /// The service that owns the sequential file implementations.
    using stream_file_service_type = detail::posix_stream_file_service;
    /// The concrete random-access file type.
    using random_access_file_type = detail::posix_random_access_file;
    /// The service that owns the random-access file implementations.
    using random_access_file_service_type =
        detail::posix_random_access_file_service;

    /** Create the scheduler and services for this backend.

        @param ctx The execution context that owns the scheduler.
        @param concurrency_hint Hint for the number of threads that
            call `run()`. A performance tuning knob; the
            thread-safety contract is set separately by
            @ref io_context_options::locking.

        @return Reference to the newly created scheduler.

        @throws std::system_error If the backend's infrastructure
            could not be created.
    */
    BOOST_COROSIO_DECL static detail::scheduler&
    construct(capy::execution_context& ctx, unsigned concurrency_hint);
};

/// Tag value for selecting the kqueue backend.
inline constexpr kqueue_t kqueue{};

#endif // BOOST_COROSIO_HAS_KQUEUE

#if BOOST_COROSIO_HAS_URING

namespace detail {

class uring_tcp_socket;
class uring_tcp_service;
class uring_udp_socket;
class uring_udp_service;
class uring_tcp_acceptor;
class uring_tcp_acceptor_service;
class uring_local_stream_socket;
class uring_local_stream_service;
class uring_local_stream_acceptor;
class uring_local_stream_acceptor_service;
class uring_local_datagram_socket;
class uring_local_datagram_service;
class uring_stream_file;
class uring_stream_file_service;
class uring_random_access_file;
class uring_random_access_file_service;
class uring_scheduler;

class posix_signal;
class posix_signal_service;
class posix_resolver;
class posix_resolver_service;

} // namespace detail

/// Selects the Linux io_uring proactor as the backend.
struct uring_t
{
    /// The scheduler that drives the event loop.
    using scheduler_type = detail::uring_scheduler;
    /// The concrete TCP socket type.
    using tcp_socket_type = detail::uring_tcp_socket;
    /// The service that owns the TCP socket implementations.
    using tcp_service_type = detail::uring_tcp_service;
    /// The concrete UDP socket type.
    using udp_socket_type = detail::uring_udp_socket;
    /// The service that owns the UDP socket implementations.
    using udp_service_type = detail::uring_udp_service;
    /// The concrete TCP acceptor type.
    using tcp_acceptor_type = detail::uring_tcp_acceptor;
    /// The service that owns the TCP acceptor implementations.
    using tcp_acceptor_service_type = detail::uring_tcp_acceptor_service;

    /// The concrete Unix domain stream socket type.
    using local_stream_socket_type = detail::uring_local_stream_socket;
    /// The service that owns the Unix domain stream implementations.
    using local_stream_service_type = detail::uring_local_stream_service;
    /// The concrete Unix domain stream acceptor type.
    using local_stream_acceptor_type = detail::uring_local_stream_acceptor;
    /// The service that owns the Unix domain acceptor implementations.
    using local_stream_acceptor_service_type =
        detail::uring_local_stream_acceptor_service;
    /// The concrete Unix domain datagram socket type.
    using local_datagram_socket_type = detail::uring_local_datagram_socket;
    /// The service that owns the Unix domain datagram implementations.
    using local_datagram_service_type = detail::uring_local_datagram_service;

    /// The concrete signal set type.
    using signal_type = detail::posix_signal;
    /// The service that owns the signal set implementations.
    using signal_service_type = detail::posix_signal_service;
    /// The concrete name resolver type.
    using resolver_type = detail::posix_resolver;
    /// The service that owns the resolver implementations.
    using resolver_service_type = detail::posix_resolver_service;

    /// The concrete sequential file type.
    using stream_file_type = detail::uring_stream_file;
    /// The service that owns the sequential file implementations.
    using stream_file_service_type = detail::uring_stream_file_service;
    /// The concrete random-access file type.
    using random_access_file_type = detail::uring_random_access_file;
    /// The service that owns the random-access file implementations.
    using random_access_file_service_type =
        detail::uring_random_access_file_service;

    /** Create the scheduler and services for this backend.

        @param ctx The execution context that owns the scheduler.
        @param concurrency_hint Hint for the number of threads that
            call `run()`. A performance tuning knob; the
            thread-safety contract is set separately by
            @ref io_context_options::locking.

        @return Reference to the newly created scheduler.

        @throws std::system_error If the backend's infrastructure
            could not be created.
    */
    BOOST_COROSIO_DECL static detail::scheduler&
    construct(capy::execution_context& ctx, unsigned concurrency_hint);
};

/// Tag value for selecting the io_uring backend.
inline constexpr uring_t uring{};

#endif // BOOST_COROSIO_HAS_URING

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

/** Selects the Windows I/O Completion Ports multiplexer as the backend.

    Used for all I/O services, including TCP, UDP, Unix domain
    sockets (AF_UNIX), signals, name resolution, and file I/O.
*/
struct iocp_t
{
    /// The scheduler that drives the event loop.
    using scheduler_type = detail::win_scheduler;
    /// The concrete TCP socket type.
    using tcp_socket_type = detail::win_tcp_socket;
    /// The service that owns the TCP socket implementations.
    using tcp_service_type = detail::win_tcp_service;
    /// The concrete TCP acceptor type.
    using tcp_acceptor_type = detail::win_tcp_acceptor;
    /// The service that owns the TCP acceptor implementations.
    using tcp_acceptor_service_type = detail::win_tcp_acceptor_service;
    /// The concrete UDP socket type.
    using udp_socket_type = detail::win_udp_socket;
    /// The service that owns the UDP socket implementations.
    using udp_service_type = detail::win_udp_service;

    /// @name Unix domain socket types
    /// @{
    using local_stream_socket_type = detail::win_local_stream_socket;
    /// The service that owns the Unix domain stream implementations.
    using local_stream_service_type = detail::win_local_stream_service;
    /// The concrete Unix domain stream acceptor type.
    using local_stream_acceptor_type = detail::win_local_stream_acceptor;
    /// The service that owns the Unix domain acceptor implementations.
    using local_stream_acceptor_service_type =
        detail::win_local_stream_acceptor_service;
    /// @}

    /// The concrete signal set type.
    using signal_type = detail::win_signal;
    /// The service that owns the signal set implementations.
    using signal_service_type = detail::win_signals;
    /// The concrete name resolver type.
    using resolver_type = detail::win_resolver;
    /// The service that owns the resolver implementations.
    using resolver_service_type = detail::win_resolver_service;

    /// The concrete sequential file type.
    using stream_file_type = detail::win_stream_file;
    /// The service that owns the sequential file implementations.
    using stream_file_service_type = detail::win_file_service;
    /// The concrete random-access file type.
    using random_access_file_type = detail::win_random_access_file;
    /// The service that owns the random-access file implementations.
    using random_access_file_service_type =
        detail::win_random_access_file_service;

    /** Create the scheduler and services for this backend.

        @param ctx The execution context that owns the scheduler.
        @param concurrency_hint Hint for the number of threads that
            call `run()`. A performance tuning knob; the
            thread-safety contract is set separately by
            @ref io_context_options::locking.

        @return Reference to the newly created scheduler.

        @throws std::system_error If the backend's infrastructure
            could not be created.
    */
    BOOST_COROSIO_DECL static detail::scheduler&
    construct(capy::execution_context& ctx, unsigned concurrency_hint);
};

/// Tag value for selecting the IOCP backend.
inline constexpr iocp_t iocp{};

#endif // BOOST_COROSIO_HAS_IOCP

} // namespace boost::corosio

#endif // BOOST_COROSIO_BACKEND_HPP
