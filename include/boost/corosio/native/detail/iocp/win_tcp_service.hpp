//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_mutex.hpp>
#include <boost/corosio/native/detail/iocp/win_wsa_init.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <boost/corosio/native/detail/iocp/win_tcp_socket.hpp>

#include <MSWSock.h>

namespace boost::corosio::detail {

class win_scheduler;
class win_tcp_acceptor;
class win_tcp_acceptor_internal;
class win_tcp_acceptor_service;

/** Windows IOCP socket management service.

    This service owns all socket implementations and coordinates their
    lifecycle with the IOCP. It provides:

    - Socket implementation allocation and deallocation
    - IOCP handle association for sockets
    - Function pointer loading for ConnectEx/AcceptEx
    - Graceful shutdown - destroys all implementations when io_context stops

    @par Thread Safety
    All public member functions are thread-safe.

    @note Only available on Windows platforms.
*/
class BOOST_COROSIO_DECL win_tcp_service final
    : private win_wsa_init
    , public capy::execution_context::service
    , public io_object::io_service
{
public:
    using key_type = win_tcp_service;

    io_object::implementation* construct() override;

    void destroy(io_object::implementation* p) override;

    void close(io_object::handle& h) override;

    /** Construct the socket service.

        Obtains the IOCP handle from the scheduler service and
        loads extension function pointers.

        @param ctx Reference to the owning execution_context.
    */
    explicit win_tcp_service(capy::execution_context& ctx);

    /** Destroy the socket service. */
    ~win_tcp_service();

    win_tcp_service(win_tcp_service const&)            = delete;
    win_tcp_service& operator=(win_tcp_service const&) = delete;

    /** Shut down the service. */
    void shutdown() override;

    /** Destroy a socket implementation wrapper.
        Removes from tracking list and deletes.
    */
    void destroy_impl(win_tcp_socket& impl);

    /** Unregister a socket implementation from the service list.
        Called by the internal impl destructor.
    */
    void unregister_impl(win_tcp_socket_internal& impl);

    /** Create and register a socket with the IOCP.

        @param impl The socket implementation internal to initialize.
        @return Error code, or success.
    */
    std::error_code
    open_socket(win_tcp_socket_internal& impl, int family, int type, int protocol);

    /** Bind a stream socket to a local endpoint.

        @param impl The socket implementation internal to bind.
        @param ep The local endpoint to bind to.
        @return Error code, or success.
    */
    std::error_code
    bind_socket(win_tcp_socket_internal& impl, endpoint ep);

    /** Destroy an acceptor implementation wrapper.
        Removes from tracking list and deletes.
    */
    void destroy_acceptor_impl(win_tcp_acceptor& impl);

    /** Unregister an acceptor implementation from the service list.
        Called by the internal impl destructor.
    */
    void unregister_acceptor_impl(win_tcp_acceptor_internal& impl);

    /** Create an acceptor socket without binding or listening.

        Creates a socket and associates it with the IOCP.
        For IPv6, dual-stack is enabled by default.
        Does not set SO_REUSEADDR.

        @param impl The acceptor implementation internal to initialize.
        @param family Address family (e.g. `AF_INET`, `AF_INET6`).
        @param type Socket type (e.g. `SOCK_STREAM`).
        @param protocol Protocol number (e.g. `IPPROTO_TCP`).
        @return Error code, or success.
    */
    std::error_code open_acceptor_socket(
        win_tcp_acceptor_internal& impl, int family, int type, int protocol);

    /** Bind an open acceptor to a local endpoint.

        @param impl The acceptor implementation internal.
        @param ep The local endpoint to bind to.
        @return Error code, or success.
    */
    std::error_code bind_acceptor(win_tcp_acceptor_internal& impl, endpoint ep);

    /** Start listening for incoming connections.

        @param impl The acceptor implementation internal.
        @param backlog The listen backlog.
        @return Error code, or success.
    */
    std::error_code listen_acceptor(win_tcp_acceptor_internal& impl, int backlog);

    /** Return the IOCP handle. */
    void* native_handle() const noexcept;

    /** Return the ConnectEx function pointer. */
    LPFN_CONNECTEX connect_ex() const noexcept
    {
        return connect_ex_;
    }

    /** Return the AcceptEx function pointer. */
    LPFN_ACCEPTEX accept_ex() const noexcept
    {
        return accept_ex_;
    }

    /** Post an overlapped operation for completion. */
    void post(overlapped_op* op);

    /** Signal that an overlapped I/O is now pending (CAS protocol). */
    void on_pending(overlapped_op* op) noexcept;

    /** Post an immediate completion with pre-stored results. */
    void on_completion(overlapped_op* op, DWORD error, DWORD bytes) noexcept;

    /** Notify scheduler of pending I/O work. */
    void work_started() noexcept;

    /** Notify scheduler that I/O work completed. */
    void work_finished() noexcept;

    /** Return the owning IOCP scheduler. */
    win_scheduler& scheduler() noexcept
    {
        return sched_;
    }

private:
    friend class win_tcp_acceptor_service;

    void load_extension_functions();

    win_scheduler& sched_;
    BOOST_COROSIO_MSVC_WARNING_PUSH
    BOOST_COROSIO_MSVC_WARNING_DISABLE(4251) // detail:: members, dll-interface
    win_mutex mutex_;
    intrusive_list<win_tcp_socket_internal> socket_list_;
    intrusive_list<win_tcp_acceptor_internal> acceptor_list_;
    intrusive_list<win_tcp_socket> socket_wrapper_list_;
    intrusive_list<win_tcp_acceptor> acceptor_wrapper_list_;
    BOOST_COROSIO_MSVC_WARNING_POP
    void* iocp_;
    LPFN_CONNECTEX connect_ex_ = nullptr;
    LPFN_ACCEPTEX accept_ex_   = nullptr;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_SERVICE_HPP
