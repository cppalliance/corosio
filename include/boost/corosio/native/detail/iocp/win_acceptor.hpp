//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_ACCEPTOR_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_ACCEPTOR_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <coroutine>
#include <memory>

#include <WS2tcpip.h>
#include <MSWSock.h>

namespace boost::corosio::detail {

class win_acceptor_service;
class win_sockets;
class win_socket;
class win_acceptor_internal;

/** Accept operation state. */
struct accept_op : overlapped_op
{
    SOCKET accepted_socket   = INVALID_SOCKET;
    win_socket* peer_wrapper = nullptr;
    std::shared_ptr<win_acceptor_internal> acceptor_ptr;
    SOCKET listen_socket                 = INVALID_SOCKET;
    io_object::implementation** impl_out = nullptr;
    char addr_buf[2 * (sizeof(sockaddr_in6) + 16)];

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    accept_op() noexcept;
};

/** Internal acceptor state for IOCP-based I/O.

    This class contains the actual state for a listening socket, including
    the native socket handle and pending accept operation.

    @note Internal implementation detail. Users interact with acceptor class.
*/
class win_acceptor_internal
    : public intrusive_list<win_acceptor_internal>::node
    , public std::enable_shared_from_this<win_acceptor_internal>
{
    friend class win_sockets;
    friend class win_acceptor;

public:
    explicit win_acceptor_internal(win_sockets& svc) noexcept;
    ~win_acceptor_internal();

    /// Return the owning socket service.
    win_sockets& socket_service() noexcept;

    std::coroutine_handle<> accept(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::stop_token,
        std::error_code*,
        io_object::implementation**);

    SOCKET native_handle() const noexcept;
    endpoint local_endpoint() const noexcept;
    bool is_open() const noexcept;
    void cancel() noexcept;
    void close_socket() noexcept;
    void set_local_endpoint(endpoint ep) noexcept;

    accept_op acc_;

private:
    win_sockets& svc_;
    SOCKET socket_ = INVALID_SOCKET;
    endpoint local_endpoint_;
};

/** Acceptor implementation wrapper for IOCP-based I/O.

    This class is the public-facing implementation that holds a shared_ptr
    to the internal state. The shared_ptr is hidden from the public interface.

    @note Internal implementation detail. Users interact with acceptor class.
*/
class win_acceptor final
    : public tcp_acceptor::implementation
    , public intrusive_list<win_acceptor>::node
{
    std::shared_ptr<win_acceptor_internal> internal_;

public:
    explicit win_acceptor(
        std::shared_ptr<win_acceptor_internal> internal) noexcept;

    void close_internal() noexcept;

    std::coroutine_handle<> accept(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        std::stop_token token,
        std::error_code* ec,
        io_object::implementation** impl_out) override;

    endpoint local_endpoint() const noexcept override;
    bool is_open() const noexcept override;
    void cancel() noexcept override;

    win_acceptor_internal* get_internal() const noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_ACCEPTOR_HPP
