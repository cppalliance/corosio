//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_LOCAL_DATAGRAM_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_LOCAL_DATAGRAM_SOCKET_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/native/detail/reactor/reactor_datagram_socket.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op.hpp>
#include <boost/corosio/native/detail/select/select_op.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/capy/ex/executor_ref.hpp>

namespace boost::corosio::detail {

// Forward declarations
class select_local_datagram_service;
class select_local_datagram_socket;

/// Base operation for local datagram sockets on select.
struct select_local_datagram_op
    : reactor_op<select_local_datagram_socket, select_tcp_acceptor>
{
    void operator()() override;
};

/// Connect operation for local datagram sockets.
struct select_local_dgram_connect_op final
    : reactor_connect_op<select_local_datagram_op, corosio::local_endpoint>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// Send-to operation for local datagram sockets.
struct select_local_send_to_op final
    : reactor_send_to_op<select_local_datagram_op>
{
    void cancel() noexcept override;
};

/// Recv-from operation for local datagram sockets.
struct select_local_recv_from_op final
    : reactor_recv_from_op<select_local_datagram_op, corosio::local_endpoint>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// Connected send operation for local datagram sockets.
struct select_local_dgram_send_op final
    : reactor_send_op<select_local_datagram_op>
{
    void cancel() noexcept override;
};

/// Connected recv operation for local datagram sockets.
struct select_local_dgram_recv_op final
    : reactor_recv_op<select_local_datagram_op>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// Datagram socket implementation for local (Unix) sockets on select.
class select_local_datagram_socket final
    : public reactor_datagram_socket<
          select_local_datagram_socket,
          select_local_datagram_service,
          select_local_dgram_connect_op,
          select_local_send_to_op,
          select_local_recv_from_op,
          select_local_dgram_send_op,
          select_local_dgram_recv_op,
          select_descriptor_state,
          local_datagram_socket::implementation,
          corosio::local_endpoint>
{
    friend class select_local_datagram_service;

public:
    explicit select_local_datagram_socket(
        select_local_datagram_service& svc) noexcept;
    ~select_local_datagram_socket() override;

    std::coroutine_handle<> connect(
        std::coroutine_handle<>,
        capy::executor_ref,
        corosio::local_endpoint,
        std::stop_token,
        std::error_code*) override;

    std::coroutine_handle<> send_to(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        corosio::local_endpoint,
        int flags,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    std::coroutine_handle<> recv_from(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        corosio::local_endpoint*,
        int flags,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    std::coroutine_handle<> send(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        int flags,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    std::coroutine_handle<> recv(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        int flags,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    std::error_code shutdown(
        local_datagram_socket::shutdown_type what) noexcept override
    {
        return do_shutdown(static_cast<int>(what));
    }

    std::error_code bind(corosio::local_endpoint ep) noexcept override
    {
        return do_bind(ep);
    }

    corosio::local_endpoint remote_endpoint() const noexcept override
    {
        return reactor_datagram_socket::remote_endpoint();
    }

    void cancel() noexcept override;
    void close_socket() noexcept;
    native_handle_type release_socket() noexcept override;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_LOCAL_DATAGRAM_SOCKET_HPP
