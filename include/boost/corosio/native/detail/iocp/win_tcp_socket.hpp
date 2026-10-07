//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_SOCKET_HPP

#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <coroutine>

#include <mswsock.h>

namespace boost::corosio::detail {

class win_tcp_service;
class win_tcp_socket;

/** Connect operation state. */
struct connect_op : overlapped_op
{
    win_tcp_socket& internal;
    endpoint target_endpoint;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit connect_op(win_tcp_socket& internal_) noexcept;
};

/** Read operation state with buffer descriptors. */
struct read_op : overlapped_op
{
    static constexpr std::size_t max_buffers = 16;
    WSABUF wsabufs[max_buffers];
    DWORD wsabuf_count = 0;
    DWORD flags        = 0;
    win_tcp_socket& internal;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit read_op(win_tcp_socket& internal_) noexcept;
};

/** Write operation state with buffer descriptors. */
struct write_op : overlapped_op
{
    static constexpr std::size_t max_buffers = 16;
    WSABUF wsabufs[max_buffers];
    DWORD wsabuf_count = 0;
    win_tcp_socket& internal;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit write_op(win_tcp_socket& internal_) noexcept;
};

/** Readiness-wait operation state.

    Completion conveys an error_code only (no bytes_transferred).
    wait_type::read posts a zero-byte WSARecv: the kernel signals
    completion when data arrives without consuming it.
    wait_type::write and wait_type::error park the op in the
    auxiliary poll reactor until the socket becomes writable or the
    kernel reports an error condition.
*/
struct wait_op : overlapped_op
{
    WSABUF wsabuf{};
    DWORD flags = 0;
    wait_type w = wait_type::read;
    win_tcp_socket& internal;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit wait_op(win_tcp_socket& internal_) noexcept;
};

/** Socket implementation for IOCP-based I/O.

    Collapses the historical internal-state/wrapper split into one
    pooled `io_object::implementation`: it both owns the native socket
    handle and pending operations, and directly implements
    `tcp_socket::implementation`. `win_tcp_service` recycles instances
    through its pool instead of freeing them on every close, so the
    keepalive each embedded op holds is the intrusive `object_ref` from
    `coro_op`, not a `shared_ptr`.

    @note Internal implementation detail. Users interact with socket class.
*/
class win_tcp_socket final
    : public tcp_socket::implementation
    , public intrusive_list<win_tcp_socket>::node
{
    friend class win_tcp_service;
    friend struct read_op;
    friend struct write_op;
    friend struct connect_op;
    friend struct wait_op;

    win_tcp_service& svc_;
    connect_op conn_;
    read_op rd_;
    write_op wr_;
    wait_op wt_;
    SOCKET socket_ = INVALID_SOCKET;
    int family_    = AF_UNSPEC;
    endpoint local_endpoint_;
    endpoint remote_endpoint_;

public:
    explicit win_tcp_socket(win_tcp_service& svc) noexcept;

    /// Recycle into the owning service's pool. Defined out-of-line
    /// after `win_tcp_service` is complete (needs `svc_.pool_`).
    void retire() noexcept override;

    /** Reset recycled state for reuse.

        `close_socket()` already drives the socket handle and family
        to their closed values, and each op's completion drops its
        `object_ref_` before `refs_` can reach zero. The endpoints are
        reset here: a connect completing on another thread can write
        them after the close. Each op's `OVERLAPPED` fields are
        re-seeded by `overlapped_op::reset()` at its next submit, so
        they are not zeroed here.

        @pre refs_ == 0, socket closed, no op in flight.
    */
    void reuse() noexcept;

    std::coroutine_handle<> connect(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        endpoint ep,
        std::stop_token token,
        std::error_code* ec) override;

    std::coroutine_handle<> read_some(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        buffer_param buf,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override;

    std::coroutine_handle<> write_some(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        buffer_param buf,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override;

    std::coroutine_handle<> wait(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        wait_type w,
        std::stop_token token,
        std::error_code* ec) override;

    std::error_code shutdown(tcp_socket::shutdown_type what) noexcept override;

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

    endpoint local_endpoint() const noexcept override;
    endpoint remote_endpoint() const noexcept override;
    void cancel() noexcept override;

    /// Used by the acceptor's `accept_op` to finish setting up the peer.
    void set_socket(SOCKET s) noexcept;
    void set_endpoints(endpoint local, endpoint remote) noexcept;

    void close_socket() noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_SOCKET_HPP
