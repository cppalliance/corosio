//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_ACCEPTOR_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_ACCEPTOR_HPP

#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <coroutine>

#include <ws2tcpip.h>
#include <mswsock.h>

namespace boost::corosio::detail {

class win_tcp_acceptor_service;
class win_tcp_service;
class win_tcp_socket;
class win_tcp_acceptor;

/** Accept operation state. */
struct accept_op : overlapped_op
{
    SOCKET accepted_socket       = INVALID_SOCKET;
    win_tcp_socket* peer_wrapper = nullptr;
    win_tcp_acceptor& acceptor;
    SOCKET listen_socket                 = INVALID_SOCKET;
    io_object::implementation** impl_out = nullptr;
    char addr_buf[2 * (sizeof(sockaddr_in6) + 16)];

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit accept_op(win_tcp_acceptor& acceptor_) noexcept;
};

/** Readiness-wait operation state for an acceptor. */
struct acceptor_wait_op : overlapped_op
{
    win_tcp_acceptor& acceptor;
    SOCKET listen_socket = INVALID_SOCKET;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit acceptor_wait_op(win_tcp_acceptor& acceptor_) noexcept;
};

/** Acceptor implementation for IOCP-based I/O.

    Collapses the historical internal-state/wrapper split into one
    pooled `io_object::implementation`: it both owns the listening
    socket and pending operations, and directly implements
    `tcp_acceptor::implementation`. `win_tcp_service` recycles
    instances through its acceptor pool instead of freeing them on
    every close.

    @note Internal implementation detail. Users interact with acceptor class.
*/
class win_tcp_acceptor final
    : public tcp_acceptor::implementation
    , public intrusive_list<win_tcp_acceptor>::node
{
    friend class win_tcp_service;
    friend struct accept_op;
    friend struct acceptor_wait_op;

    win_tcp_service& svc_;
    accept_op acc_;
    acceptor_wait_op wt_;
    SOCKET socket_ = INVALID_SOCKET;
    int family_    = AF_UNSPEC;
    endpoint local_endpoint_;

public:
    explicit win_tcp_acceptor(win_tcp_service& svc) noexcept;

    /// Recycle into the owning service's pool. Defined out-of-line
    /// after `win_tcp_service` is complete (needs `svc_.acceptor_pool_`).
    void retire() noexcept override;

    /** Reset recycled state for reuse.

        `close_socket()` already drives the socket handle, family,
        and cached endpoint to their closed values, and each op's
        completion drops its `object_ref_` before `refs_` can reach
        zero, so this only asserts. Each op's `OVERLAPPED` fields are
        re-seeded by `overlapped_op::reset()` at its next submit, so
        they are not zeroed here.

        @pre refs_ == 0, socket closed, no op in flight.
    */
    void reuse() noexcept;

    /// Return the owning socket service.
    win_tcp_service& socket_service() noexcept;

    std::coroutine_handle<> accept(
        capy::continuation& cont,
        capy::executor_ref d,
        std::stop_token token,
        std::error_code* ec,
        io_object::implementation** impl_out) override;

    std::coroutine_handle<> wait(
        capy::continuation& cont,
        capy::executor_ref d,
        wait_type w,
        std::stop_token token,
        std::error_code* ec) override;

    endpoint local_endpoint() const noexcept override;
    bool is_open() const noexcept override;
    void cancel() noexcept override;

    native_handle_type native_handle() const noexcept override;

    corosio::family family() const noexcept override
    {
        // getsockname fails with WSAEINVAL on an unbound socket, so
        // the family recorded at socket creation is authoritative
        return to_family(family_);
    }
    native_handle_type release_socket() noexcept override;

    std::error_code set_option(
        int level,
        int optname,
        void const* data,
        std::size_t size) noexcept override;
    std::error_code
    get_option(int level, int optname, void* data, std::size_t* size)
        const noexcept override;

    void set_local_endpoint(endpoint ep) noexcept;
    void close_socket() noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_ACCEPTOR_HPP
