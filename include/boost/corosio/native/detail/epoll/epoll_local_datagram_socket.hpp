//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_LOCAL_DATAGRAM_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_LOCAL_DATAGRAM_SOCKET_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/native/detail/reactor/reactor_datagram_socket.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op.hpp>
#include <boost/corosio/native/detail/epoll/epoll_op.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/capy/ex/executor_ref.hpp>

namespace boost::corosio::detail {

// Forward declarations
class epoll_local_datagram_service;
class epoll_local_datagram_socket;

/// Base operation for local datagram sockets on epoll.
struct epoll_local_datagram_op
    : reactor_op<epoll_local_datagram_socket, epoll_tcp_acceptor>
{
    void operator()() override;
};

/// Connect operation for local datagram sockets.
struct epoll_local_dgram_connect_op final
    : reactor_connect_op<epoll_local_datagram_op, corosio::local_endpoint>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// Send-to operation for local datagram sockets.
struct epoll_local_send_to_op final
    : reactor_send_to_op<epoll_local_datagram_op>
{
    void cancel() noexcept override;
};

/// Recv-from operation for local datagram sockets.
struct epoll_local_recv_from_op final
    : reactor_recv_from_op<epoll_local_datagram_op, corosio::local_endpoint>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// Connected send operation for local datagram sockets.
struct epoll_local_dgram_send_op final
    : reactor_send_op<epoll_local_datagram_op>
{
    void cancel() noexcept override;
};

/// Connected recv operation for local datagram sockets.
struct epoll_local_dgram_recv_op final
    : reactor_recv_op<epoll_local_datagram_op>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// Datagram socket implementation for local (Unix) sockets on epoll.
class epoll_local_datagram_socket final
    : public reactor_datagram_socket<
          epoll_local_datagram_socket,
          epoll_local_datagram_service,
          epoll_local_dgram_connect_op,
          epoll_local_send_to_op,
          epoll_local_recv_from_op,
          epoll_local_dgram_send_op,
          epoll_local_dgram_recv_op,
          descriptor_state,
          local_datagram_socket::implementation,
          corosio::local_endpoint>
{
    friend class epoll_local_datagram_service;

public:
    explicit epoll_local_datagram_socket(
        epoll_local_datagram_service& svc) noexcept;
    ~epoll_local_datagram_socket() override;

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

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_LOCAL_DATAGRAM_SOCKET_HPP
