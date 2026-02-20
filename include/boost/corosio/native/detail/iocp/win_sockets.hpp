//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SOCKETS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SOCKETS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_mutex.hpp>
#include <boost/corosio/native/detail/iocp/win_wsa_init.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <boost/corosio/native/detail/iocp/win_socket.hpp>

#include <MSWSock.h>

namespace boost::corosio::detail {

class win_scheduler;
class win_acceptor;
class win_acceptor_internal;
class win_acceptor_service;

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
class BOOST_COROSIO_DECL win_sockets final
    : private win_wsa_init
    , public capy::execution_context::service
    , public io_object::io_service
{
public:
    using key_type = win_sockets;

    io_object::implementation* construct() override;

    void destroy(io_object::implementation* p) override;

    void close(io_object::handle& h) override;

    /** Construct the socket service.

        Obtains the IOCP handle from the scheduler service and
        loads extension function pointers.

        @param ctx Reference to the owning execution_context.
    */
    explicit win_sockets(capy::execution_context& ctx);

    /** Destroy the socket service. */
    ~win_sockets();

    win_sockets(win_sockets const&)            = delete;
    win_sockets& operator=(win_sockets const&) = delete;

    /** Shut down the service. */
    void shutdown() override;

    /** Destroy a socket implementation wrapper.
        Removes from tracking list and deletes.
    */
    void destroy_impl(win_socket& impl);

    /** Unregister a socket implementation from the service list.
        Called by the internal impl destructor.
    */
    void unregister_impl(win_socket_internal& impl);

    /** Create and register a socket with the IOCP.

        @param impl The socket implementation internal to initialize.
        @return Error code, or success.
    */
    std::error_code open_socket(win_socket_internal& impl);

    /** Destroy an acceptor implementation wrapper.
        Removes from tracking list and deletes.
    */
    void destroy_acceptor_impl(win_acceptor& impl);

    /** Unregister an acceptor implementation from the service list.
        Called by the internal impl destructor.
    */
    void unregister_acceptor_impl(win_acceptor_internal& impl);

    /** Create, bind, and listen on an acceptor socket.

        @param impl The acceptor implementation internal to initialize.
        @param ep The local endpoint to bind to.
        @param backlog The listen backlog.
        @return Error code, or success.
    */
    std::error_code
    open_acceptor(win_acceptor_internal& impl, endpoint ep, int backlog);

    /** Return the IOCP handle. */
    void* native_handle() const noexcept;

    /** Return the ConnectEx function pointer. */
    LPFN_CONNECTEX connect_ex() const noexcept;

    /** Return the AcceptEx function pointer. */
    LPFN_ACCEPTEX accept_ex() const noexcept;

    /** Post an overlapped operation for completion. */
    void post(overlapped_op* op);

    /** Notify scheduler of pending I/O work. */
    void work_started() noexcept;

    /** Notify scheduler that I/O work completed. */
    void work_finished() noexcept;

private:
    friend class win_acceptor_service;

    void load_extension_functions();

    win_scheduler& sched_;
    win_mutex mutex_;
    intrusive_list<win_socket_internal> socket_list_;
    intrusive_list<win_acceptor_internal> acceptor_list_;
    intrusive_list<win_socket> socket_wrapper_list_;
    intrusive_list<win_acceptor> acceptor_wrapper_list_;
    void* iocp_;
    LPFN_CONNECTEX connect_ex_ = nullptr;
    LPFN_ACCEPTEX accept_ex_   = nullptr;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SOCKETS_HPP
