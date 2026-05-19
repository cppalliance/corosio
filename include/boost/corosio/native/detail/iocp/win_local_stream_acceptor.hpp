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
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>
#include <boost/corosio/native/detail/endpoint_convert.hpp>

#include <coroutine>
#include <memory>

#include <WS2tcpip.h>
#include <MSWSock.h>

namespace boost::corosio::detail {

class win_local_stream_acceptor_service;
class win_local_stream_service;
class win_local_stream_socket;
class win_local_stream_acceptor_internal;

/** Accept operation state for local stream sockets.

    The addr_buf is sized for sockaddr_un (larger than sockaddr_in6).
*/
struct local_stream_accept_op : overlapped_op
{
    SOCKET accepted_socket   = INVALID_SOCKET;
    win_local_stream_socket* peer_wrapper = nullptr;
    std::shared_ptr<win_local_stream_acceptor_internal> acceptor_ptr;
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

    local_stream_accept_op() noexcept;
};

/** Readiness-wait operation state for a local stream acceptor. */
struct local_stream_acceptor_wait_op : overlapped_op
{
    std::shared_ptr<win_local_stream_acceptor_internal> acceptor_ptr;
    SOCKET listen_socket = INVALID_SOCKET;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    local_stream_acceptor_wait_op() noexcept;
};

/* Internal acceptor state for IOCP local stream I/O. */
class win_local_stream_acceptor_internal
    : public intrusive_list<win_local_stream_acceptor_internal>::node
    , public std::enable_shared_from_this<win_local_stream_acceptor_internal>
{
    friend class win_local_stream_service;
    friend class win_local_stream_acceptor;

public:
    explicit win_local_stream_acceptor_internal(
        win_local_stream_service& svc) noexcept;
    ~win_local_stream_acceptor_internal();

    win_local_stream_service& socket_service() noexcept;

    std::coroutine_handle<> accept(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::stop_token,
        std::error_code*,
        io_object::implementation**);

    std::coroutine_handle<> wait(
        std::coroutine_handle<>,
        capy::executor_ref,
        wait_type,
        std::stop_token,
        std::error_code*);

    SOCKET native_handle() const noexcept;
    corosio::local_endpoint local_endpoint() const noexcept;
    bool is_open() const noexcept;
    void cancel() noexcept;
    void close_socket() noexcept;
    void set_local_endpoint(corosio::local_endpoint ep) noexcept;

    local_stream_accept_op acc_;
    local_stream_acceptor_wait_op wt_;

private:
    win_local_stream_service& svc_;
    SOCKET socket_ = INVALID_SOCKET;
    corosio::local_endpoint local_endpoint_;
};

/* Acceptor implementation wrapper for IOCP local stream I/O. */
class win_local_stream_acceptor final
    : public local_stream_acceptor::implementation
    , public intrusive_list<win_local_stream_acceptor>::node
{
    std::shared_ptr<win_local_stream_acceptor_internal> internal_;

public:
    explicit win_local_stream_acceptor(
        std::shared_ptr<win_local_stream_acceptor_internal> internal) noexcept;

    void close_internal() noexcept;

    std::coroutine_handle<> accept(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        std::stop_token token,
        std::error_code* ec,
        io_object::implementation** impl_out) override;

    std::coroutine_handle<> wait(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        wait_type w,
        std::stop_token token,
        std::error_code* ec) override;

    corosio::local_endpoint local_endpoint() const noexcept override;
    bool is_open() const noexcept override;
    void cancel() noexcept override;

    native_handle_type release_socket() noexcept override;

    std::error_code set_option(
        int level,
        int optname,
        void const* data,
        std::size_t size) noexcept override;
    std::error_code
    get_option(int level, int optname, void* data, std::size_t* size)
        const noexcept override;

    win_local_stream_acceptor_internal* get_internal() const noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_STREAM_ACCEPTOR_HPP
