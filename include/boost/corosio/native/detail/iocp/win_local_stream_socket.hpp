//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_STREAM_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_STREAM_SOCKET_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <coroutine>

#include <mswsock.h>

namespace boost::corosio::detail {

class win_local_stream_service;
class win_local_stream_socket;

/** Connect operation state for local stream sockets. */
struct local_stream_connect_op : overlapped_op
{
    win_local_stream_socket& internal;
    corosio::local_endpoint target_endpoint;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit local_stream_connect_op(
        win_local_stream_socket& internal_) noexcept;
};

/** Read operation state for local stream sockets. */
struct local_stream_read_op : overlapped_op
{
    static constexpr std::size_t max_buffers = 16;
    WSABUF wsabufs[max_buffers];
    DWORD wsabuf_count = 0;
    DWORD flags        = 0;
    win_local_stream_socket& internal;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit local_stream_read_op(win_local_stream_socket& internal_) noexcept;
};

/** Write operation state for local stream sockets. */
struct local_stream_write_op : overlapped_op
{
    static constexpr std::size_t max_buffers = 16;
    WSABUF wsabufs[max_buffers];
    DWORD wsabuf_count = 0;
    win_local_stream_socket& internal;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit local_stream_write_op(win_local_stream_socket& internal_) noexcept;
};

/** Readiness-wait operation state for local stream sockets. */
struct local_stream_wait_op : overlapped_op
{
    WSABUF wsabuf{};
    DWORD flags = 0;
    wait_type w = wait_type::read;
    win_local_stream_socket& internal;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit local_stream_wait_op(win_local_stream_socket& internal_) noexcept;
};

/** Socket implementation for IOCP local stream I/O.

   Collapses the historical internal-state/wrapper split into one
   pooled `io_object::implementation`: it both owns the native SOCKET
   handle and pending operations, and directly implements
   `local_stream_socket::implementation`. `win_local_stream_service`
   recycles instances through its pool instead of freeing them on
   every close.
*/
class win_local_stream_socket final
    : public local_stream_socket::implementation
    , public intrusive_list<win_local_stream_socket>::node
{
    friend class win_local_stream_service;
    friend struct local_stream_read_op;
    friend struct local_stream_write_op;
    friend struct local_stream_connect_op;
    friend struct local_stream_wait_op;

    win_local_stream_service& svc_;
    local_stream_connect_op conn_;
    local_stream_read_op rd_;
    local_stream_write_op wr_;
    local_stream_wait_op wt_;
    SOCKET socket_ = INVALID_SOCKET;
    corosio::local_endpoint local_endpoint_;
    corosio::local_endpoint remote_endpoint_;

public:
    explicit win_local_stream_socket(win_local_stream_service& svc) noexcept;

    /// Recycle into the owning service's pool. Defined out-of-line
    /// after `win_local_stream_service` is complete (needs
    /// `svc_.pool_`).
    void retire() noexcept override;

    /** Reset recycled state for reuse.

        Each op's `OVERLAPPED` fields are re-seeded by
        `overlapped_op::reset()` at its next submit, so they are not
        zeroed here.

        @pre refs_ == 0, socket closed, no op in flight.
    */
    void reuse() noexcept;

    std::coroutine_handle<> connect(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        corosio::local_endpoint ep,
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

    std::error_code
    shutdown(local_stream_socket::shutdown_type what) noexcept override;

    native_handle_type native_handle() const noexcept override;

    corosio::family family() const noexcept override
    {
        // Local sockets have no IP family; v4 is the inert value
        return corosio::family::v4;
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

    corosio::local_endpoint local_endpoint() const noexcept override;
    corosio::local_endpoint remote_endpoint() const noexcept override;
    void cancel() noexcept override;

    /// Used by the acceptor's `local_stream_accept_op` to finish
    /// setting up the peer.
    void set_socket(SOCKET s) noexcept;
    void set_endpoints(
        corosio::local_endpoint local, corosio::local_endpoint remote) noexcept;

    void close_socket() noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_STREAM_SOCKET_HPP
