//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SOCKET_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/detail/cached_initiator.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <coroutine>
#include <memory>

#include <MSWSock.h>

namespace boost::corosio::detail {

class win_sockets;
class win_socket_internal;

/** Connect operation state. */
struct connect_op : overlapped_op
{
    win_socket_internal& internal;
    std::shared_ptr<win_socket_internal> internal_ptr;
    endpoint target_endpoint;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit connect_op(win_socket_internal& internal_) noexcept;
};

/** Read operation state with buffer descriptors. */
struct read_op : overlapped_op
{
    static constexpr std::size_t max_buffers = 16;
    WSABUF wsabufs[max_buffers];
    DWORD wsabuf_count = 0;
    DWORD flags        = 0;
    win_socket_internal& internal;
    std::shared_ptr<win_socket_internal> internal_ptr;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit read_op(win_socket_internal& internal_) noexcept;
};

/** Write operation state with buffer descriptors. */
struct write_op : overlapped_op
{
    static constexpr std::size_t max_buffers = 16;
    WSABUF wsabufs[max_buffers];
    DWORD wsabuf_count = 0;
    win_socket_internal& internal;
    std::shared_ptr<win_socket_internal> internal_ptr;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit write_op(win_socket_internal& internal_) noexcept;
};

/** Internal socket state for IOCP-based I/O.

    This class contains the actual state for a single socket, including
    the native socket handle and pending operations. It derives from
    enable_shared_from_this so operations can extend its lifetime.

    @note Internal implementation detail. Users interact with socket class.
*/
class win_socket_internal
    : public intrusive_list<win_socket_internal>::node
    , public std::enable_shared_from_this<win_socket_internal>
{
    friend class win_sockets;
    friend class win_socket;
    friend struct read_op;
    friend struct write_op;
    friend struct connect_op;

    win_sockets& svc_;
    connect_op conn_;
    read_op rd_;
    write_op wr_;
    SOCKET socket_ = INVALID_SOCKET;

    cached_initiator read_initiator_;
    cached_initiator write_initiator_;

public:
    explicit win_socket_internal(win_sockets& svc) noexcept;
    ~win_socket_internal();

    std::coroutine_handle<> connect(
        std::coroutine_handle<>,
        capy::executor_ref,
        endpoint,
        std::stop_token,
        std::error_code*);

    std::coroutine_handle<> read_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        io_buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*);

    std::coroutine_handle<> write_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        io_buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*);

    SOCKET native_handle() const noexcept;
    endpoint local_endpoint() const noexcept;
    endpoint remote_endpoint() const noexcept;
    bool is_open() const noexcept;
    void cancel() noexcept;
    void close_socket() noexcept;
    void set_socket(SOCKET s) noexcept;
    void set_endpoints(endpoint local, endpoint remote) noexcept;

    /** Execute the read I/O operation (called by initiator coroutine). */
    void do_read_io();

    /** Execute the write I/O operation (called by initiator coroutine). */
    void do_write_io();

private:
    endpoint local_endpoint_;
    endpoint remote_endpoint_;
};

/** Socket implementation wrapper for IOCP-based I/O.

    This class is the public-facing implementation that holds a shared_ptr
    to the internal state. The shared_ptr is hidden from the public interface.

    @note Internal implementation detail. Users interact with socket class.
*/
class win_socket final
    : public tcp_socket::implementation
    , public intrusive_list<win_socket>::node
{
    std::shared_ptr<win_socket_internal> internal_;

public:
    explicit win_socket(std::shared_ptr<win_socket_internal> internal) noexcept;

    void close_internal() noexcept;

    std::coroutine_handle<> connect(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        endpoint ep,
        std::stop_token token,
        std::error_code* ec) override;

    std::coroutine_handle<> read_some(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        io_buffer_param buf,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override;

    std::coroutine_handle<> write_some(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        io_buffer_param buf,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override;

    std::error_code shutdown(tcp_socket::shutdown_type what) noexcept override;

    native_handle_type native_handle() const noexcept override;

    std::error_code set_no_delay(bool value) noexcept override;
    bool no_delay(std::error_code& ec) const noexcept override;

    std::error_code set_keep_alive(bool value) noexcept override;
    bool keep_alive(std::error_code& ec) const noexcept override;

    std::error_code set_receive_buffer_size(int size) noexcept override;
    int receive_buffer_size(std::error_code& ec) const noexcept override;

    std::error_code set_send_buffer_size(int size) noexcept override;
    int send_buffer_size(std::error_code& ec) const noexcept override;

    std::error_code set_linger(bool enabled, int timeout) noexcept override;
    tcp_socket::linger_options
    linger(std::error_code& ec) const noexcept override;

    endpoint local_endpoint() const noexcept override;
    endpoint remote_endpoint() const noexcept override;
    void cancel() noexcept override;

    win_socket_internal* get_internal() const noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SOCKET_HPP
