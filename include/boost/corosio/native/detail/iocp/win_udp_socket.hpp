//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_UDP_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_UDP_SOCKET_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <coroutine>
#include <memory>

namespace boost::corosio::detail {

class win_udp_service;
class win_udp_socket_internal;

/** Send-to operation state with buffer and destination address. */
struct send_to_op : overlapped_op
{
    static constexpr std::size_t max_buffers = 16;
    WSABUF wsabufs[max_buffers];
    DWORD wsabuf_count = 0;
    sockaddr_storage dest_storage{};
    int dest_len = 0;
    win_udp_socket_internal& internal;
    std::shared_ptr<win_udp_socket_internal> internal_ptr;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit send_to_op(win_udp_socket_internal& internal_) noexcept;
};

/** Recv-from operation state with buffer and source address capture. */
struct recv_from_op : overlapped_op
{
    static constexpr std::size_t max_buffers = 16;
    WSABUF wsabufs[max_buffers];
    DWORD wsabuf_count = 0;
    DWORD flags        = 0;
    sockaddr_storage source_storage{};
    INT source_len   = sizeof(sockaddr_storage);
    endpoint* source_out = nullptr;
    win_udp_socket_internal& internal;
    std::shared_ptr<win_udp_socket_internal> internal_ptr;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit recv_from_op(win_udp_socket_internal& internal_) noexcept;
};

/** Internal datagram socket state for IOCP-based I/O.

    This class contains the actual state for a single UDP socket,
    including the native socket handle and pending operations. It
    derives from enable_shared_from_this so operations can extend
    its lifetime.

    @note Internal implementation detail.
*/
class win_udp_socket_internal
    : public intrusive_list<win_udp_socket_internal>::node
    , public std::enable_shared_from_this<win_udp_socket_internal>
{
    friend class win_udp_service;
    friend class win_udp_socket;
    friend struct send_to_op;
    friend struct recv_from_op;

    win_udp_service& svc_;
    send_to_op wr_;
    recv_from_op rd_;
    SOCKET socket_ = INVALID_SOCKET;
    int family_    = AF_UNSPEC;

public:
    explicit win_udp_socket_internal(
        win_udp_service& svc) noexcept;
    ~win_udp_socket_internal();

    std::coroutine_handle<> send_to(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        endpoint,
        std::stop_token,
        std::error_code*,
        std::size_t*);

    std::coroutine_handle<> recv_from(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        endpoint*,
        std::stop_token,
        std::error_code*,
        std::size_t*);

    SOCKET native_handle() const noexcept;
    endpoint local_endpoint() const noexcept;
    bool is_open() const noexcept;
    void cancel() noexcept;
    void close_socket() noexcept;

private:
    endpoint local_endpoint_;
};

/** Datagram socket implementation wrapper for IOCP-based I/O.

    This class is the public-facing implementation that holds a
    shared_ptr to the internal state. The shared_ptr is hidden
    from the public interface.

    @note Internal implementation detail.
*/
class win_udp_socket final
    : public udp_socket::implementation
    , public intrusive_list<win_udp_socket>::node
{
    std::shared_ptr<win_udp_socket_internal> internal_;

public:
    explicit win_udp_socket(
        std::shared_ptr<win_udp_socket_internal> internal) noexcept;

    void close_internal() noexcept;

    std::coroutine_handle<> send_to(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        buffer_param buf,
        endpoint dest,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override;

    std::coroutine_handle<> recv_from(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        buffer_param buf,
        endpoint* source,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override;

    native_handle_type native_handle() const noexcept override;

    std::error_code set_option(
        int level,
        int optname,
        void const* data,
        std::size_t size) noexcept override;
    std::error_code
    get_option(int level, int optname, void* data, std::size_t* size)
        const noexcept override;

    endpoint local_endpoint() const noexcept override;
    void cancel() noexcept override;

    win_udp_socket_internal* get_internal() const noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_UDP_SOCKET_HPP
