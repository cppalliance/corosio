//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_STREAM_ACCEPTOR_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_STREAM_ACCEPTOR_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>
#include <boost/corosio/native/detail/endpoint_convert.hpp>

#include <coroutine>

#include <ws2tcpip.h>
#include <mswsock.h>

namespace boost::corosio::detail {

class win_local_stream_acceptor_service;
class win_local_stream_service;
class win_local_stream_socket;
class win_local_stream_acceptor;

/** Accept operation state for local stream sockets.

    The addr_buf is sized for sockaddr_un (larger than sockaddr_in6).
*/
struct local_stream_accept_op : overlapped_op
{
    SOCKET accepted_socket                = INVALID_SOCKET;
    win_local_stream_socket* peer_wrapper = nullptr;
    win_local_stream_acceptor& acceptor;
    SOCKET listen_socket                 = INVALID_SOCKET;
    io_object::implementation** impl_out = nullptr;
    // 2 * (sizeof(un_sa_t) + 16) = 2 * (110 + 16) = 252
    char addr_buf[2 * (sizeof(un_sa_t) + 16)];

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit local_stream_accept_op(win_local_stream_acceptor& acceptor_) noexcept;
};

/** Readiness-wait operation state for a local stream acceptor. */
struct local_stream_acceptor_wait_op : overlapped_op
{
    win_local_stream_acceptor& acceptor;
    SOCKET listen_socket = INVALID_SOCKET;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit local_stream_acceptor_wait_op(
        win_local_stream_acceptor& acceptor_) noexcept;
};

/** Acceptor implementation for IOCP local stream I/O.

   Collapses the historical internal-state/wrapper split into one
   pooled `io_object::implementation`.
*/
class win_local_stream_acceptor final
    : public local_stream_acceptor::implementation
    , public intrusive_list<win_local_stream_acceptor>::node
{
    friend class win_local_stream_service;
    friend struct local_stream_accept_op;
    friend struct local_stream_acceptor_wait_op;

    win_local_stream_service& svc_;
    local_stream_accept_op acc_;
    local_stream_acceptor_wait_op wt_;
    SOCKET socket_ = INVALID_SOCKET;
    corosio::local_endpoint local_endpoint_;

public:
    explicit win_local_stream_acceptor(win_local_stream_service& svc) noexcept;

    /// Recycle into the owning service's pool. Defined out-of-line
    /// after `win_local_stream_service` is complete (needs
    /// `svc_.acceptor_pool_`).
    void retire() noexcept override;

    /** Reset recycled state for reuse.

        Each op's `OVERLAPPED` fields are re-seeded by
        `overlapped_op::reset()` at its next submit, so they are not
        zeroed here.

        @pre refs_ == 0, socket closed, no op in flight.
    */
    void reuse() noexcept;

    win_local_stream_service& socket_service() noexcept;

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

    corosio::local_endpoint local_endpoint() const noexcept override;
    bool is_open() const noexcept override;
    void cancel() noexcept override;

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

    void set_local_endpoint(corosio::local_endpoint ep) noexcept;
    void close_socket() noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_STREAM_ACCEPTOR_HPP
