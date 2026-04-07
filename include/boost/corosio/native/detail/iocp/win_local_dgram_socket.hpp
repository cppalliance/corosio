//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_DGRAM_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_DGRAM_SOCKET_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <coroutine>
#include <memory>

namespace boost::corosio::detail {

class win_local_dgram_service;
class win_local_dgram_socket_internal;

/** Send-to operation for local datagram sockets. */
struct local_dgram_send_to_op : overlapped_op
{
    static constexpr std::size_t max_buffers = 16;
    WSABUF wsabufs[max_buffers];
    DWORD wsabuf_count = 0;
    sockaddr_storage dest_storage{};
    int dest_len = 0;
    win_local_dgram_socket_internal& internal;
    std::shared_ptr<win_local_dgram_socket_internal> internal_ptr;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit local_dgram_send_to_op(
        win_local_dgram_socket_internal& internal_) noexcept;
};

/** Recv-from operation for local datagram sockets. */
struct local_dgram_recv_from_op : overlapped_op
{
    static constexpr std::size_t max_buffers = 16;
    WSABUF wsabufs[max_buffers];
    DWORD wsabuf_count = 0;
    DWORD flags        = 0;
    sockaddr_storage source_storage{};
    INT source_len = sizeof(sockaddr_storage);
    corosio::local_endpoint* source_out = nullptr;
    win_local_dgram_socket_internal& internal;
    std::shared_ptr<win_local_dgram_socket_internal> internal_ptr;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit local_dgram_recv_from_op(
        win_local_dgram_socket_internal& internal_) noexcept;
};

/** Connect operation for connected-mode local datagrams. */
struct local_dgram_connect_op : overlapped_op
{
    corosio::local_endpoint target_endpoint;
    win_local_dgram_socket_internal& internal;
    std::shared_ptr<win_local_dgram_socket_internal> internal_ptr;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit local_dgram_connect_op(
        win_local_dgram_socket_internal& internal_) noexcept;
};

/** Connected send operation for local datagrams. */
struct local_dgram_send_op : overlapped_op
{
    static constexpr std::size_t max_buffers = 16;
    WSABUF wsabufs[max_buffers];
    DWORD wsabuf_count = 0;
    win_local_dgram_socket_internal& internal;
    std::shared_ptr<win_local_dgram_socket_internal> internal_ptr;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit local_dgram_send_op(
        win_local_dgram_socket_internal& internal_) noexcept;
};

/** Connected recv operation for local datagrams. */
struct local_dgram_recv_op : overlapped_op
{
    static constexpr std::size_t max_buffers = 16;
    WSABUF wsabufs[max_buffers];
    DWORD wsabuf_count = 0;
    DWORD flags        = 0;
    win_local_dgram_socket_internal& internal;
    std::shared_ptr<win_local_dgram_socket_internal> internal_ptr;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit local_dgram_recv_op(
        win_local_dgram_socket_internal& internal_) noexcept;
};

/* Internal local datagram socket state for IOCP. */
class win_local_dgram_socket_internal
    : public intrusive_list<win_local_dgram_socket_internal>::node
    , public std::enable_shared_from_this<win_local_dgram_socket_internal>
{
    friend class win_local_dgram_service;
    friend class win_local_dgram_socket;
    friend struct local_dgram_send_to_op;
    friend struct local_dgram_recv_from_op;
    friend struct local_dgram_connect_op;
    friend struct local_dgram_send_op;
    friend struct local_dgram_recv_op;

    win_local_dgram_service& svc_;
    local_dgram_send_to_op wr_;
    local_dgram_recv_from_op rd_;
    local_dgram_connect_op conn_;
    local_dgram_send_op send_wr_;
    local_dgram_recv_op recv_rd_;
    SOCKET socket_ = INVALID_SOCKET;

public:
    explicit win_local_dgram_socket_internal(
        win_local_dgram_service& svc) noexcept;
    ~win_local_dgram_socket_internal();

    std::coroutine_handle<> send_to(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        corosio::local_endpoint,
        int flags,
        std::stop_token,
        std::error_code*,
        std::size_t*);

    std::coroutine_handle<> recv_from(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        corosio::local_endpoint*,
        int flags,
        std::stop_token,
        std::error_code*,
        std::size_t*);

    std::coroutine_handle<> connect(
        std::coroutine_handle<>,
        capy::executor_ref,
        corosio::local_endpoint,
        std::stop_token,
        std::error_code*);

    std::coroutine_handle<> send(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        int flags,
        std::stop_token,
        std::error_code*,
        std::size_t*);

    std::coroutine_handle<> recv(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        int flags,
        std::stop_token,
        std::error_code*,
        std::size_t*);

    SOCKET native_handle() const noexcept;
    corosio::local_endpoint local_endpoint() const noexcept;
    corosio::local_endpoint remote_endpoint() const noexcept;
    bool is_open() const noexcept;
    void cancel() noexcept;
    void close_socket() noexcept;

private:
    corosio::local_endpoint local_endpoint_;
    corosio::local_endpoint remote_endpoint_;
};

/* Local datagram socket wrapper for IOCP. */
class win_local_dgram_socket final
    : public local_datagram_socket::implementation
    , public intrusive_list<win_local_dgram_socket>::node
{
    std::shared_ptr<win_local_dgram_socket_internal> internal_;

public:
    explicit win_local_dgram_socket(
        std::shared_ptr<win_local_dgram_socket_internal> internal) noexcept;

    void close_internal() noexcept;

    std::coroutine_handle<> send_to(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        buffer_param buf,
        corosio::local_endpoint dest,
        int flags,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override;

    std::coroutine_handle<> recv_from(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        buffer_param buf,
        corosio::local_endpoint* source,
        int flags,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override;

    std::coroutine_handle<> connect(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        corosio::local_endpoint ep,
        std::stop_token token,
        std::error_code* ec) override;

    std::coroutine_handle<> send(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        buffer_param buf,
        int flags,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override;

    std::coroutine_handle<> recv(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        buffer_param buf,
        int flags,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override;

    std::error_code bind(corosio::local_endpoint ep) noexcept override;

    std::error_code shutdown(
        local_datagram_socket::shutdown_type what) noexcept override;

    native_handle_type native_handle() const noexcept override;

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

    win_local_dgram_socket_internal* get_internal() const noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_DGRAM_SOCKET_HPP
