//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_UDP_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_UDP_SOCKET_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/native/detail/reactor/reactor_datagram_socket.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>
#include <boost/corosio/native/detail/epoll/epoll_op.hpp>
#include <boost/capy/ex/executor_ref.hpp>

namespace boost::corosio::detail {

class epoll_udp_service;
class epoll_udp_socket;

/// epoll datagram base operation.
struct epoll_datagram_op : reactor_op<epoll_udp_socket, epoll_tcp_acceptor>
{
    void operator()() override;
};

/// epoll send_to operation.
struct epoll_send_to_op final : reactor_send_to_op<epoll_datagram_op>
{
    void cancel() noexcept override;
};

/// epoll recv_from operation.
struct epoll_recv_from_op final : reactor_recv_from_op<epoll_datagram_op>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// Datagram socket implementation for epoll backend.
class epoll_udp_socket final
    : public reactor_datagram_socket<
          epoll_udp_socket,
          epoll_udp_service,
          epoll_send_to_op,
          epoll_recv_from_op,
          descriptor_state>
{
    friend class epoll_udp_service;

public:
    explicit epoll_udp_socket(epoll_udp_service& svc) noexcept;
    ~epoll_udp_socket() override;

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

    void cancel() noexcept override;
    void close_socket() noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_UDP_SOCKET_HPP
