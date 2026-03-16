//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_UDP_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_UDP_SOCKET_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/native/detail/reactor/reactor_datagram_socket.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>
#include <boost/corosio/native/detail/kqueue/kqueue_op.hpp>
#include <boost/capy/ex/executor_ref.hpp>

namespace boost::corosio::detail {

class kqueue_udp_service;
class kqueue_udp_socket;

/// kqueue datagram base operation.
struct kqueue_datagram_op : reactor_op<kqueue_udp_socket, kqueue_tcp_acceptor>
{
    void operator()() override;
};

/// kqueue send_to operation.
struct kqueue_send_to_op final : reactor_send_to_op<kqueue_datagram_op>
{
    void cancel() noexcept override;
};

/// kqueue recv_from operation.
struct kqueue_recv_from_op final : reactor_recv_from_op<kqueue_datagram_op>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// kqueue connect operation for UDP.
struct kqueue_udp_connect_op final : reactor_connect_op<kqueue_datagram_op>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// kqueue connected send operation.
struct kqueue_send_op final : reactor_send_op<kqueue_datagram_op>
{
    void cancel() noexcept override;
};

/// kqueue connected recv operation.
struct kqueue_recv_op final : reactor_recv_op<kqueue_datagram_op>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// Datagram socket implementation for kqueue backend.
class kqueue_udp_socket final
    : public reactor_datagram_socket<
          kqueue_udp_socket,
          kqueue_udp_service,
          kqueue_udp_connect_op,
          kqueue_send_to_op,
          kqueue_recv_from_op,
          kqueue_send_op,
          kqueue_recv_op,
          descriptor_state>
{
    friend class kqueue_udp_service;

public:
    explicit kqueue_udp_socket(kqueue_udp_service& svc) noexcept;
    ~kqueue_udp_socket() override;

    std::coroutine_handle<> send_to(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        endpoint,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    std::coroutine_handle<> recv_from(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        endpoint*,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    std::coroutine_handle<> connect(
        std::coroutine_handle<>,
        capy::executor_ref,
        endpoint,
        std::stop_token,
        std::error_code*) override;

    std::coroutine_handle<> send(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    std::coroutine_handle<> recv(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    endpoint remote_endpoint() const noexcept override;

    void cancel() noexcept override;
    void close_socket() noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_UDP_SOCKET_HPP
