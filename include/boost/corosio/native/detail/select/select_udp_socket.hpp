//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_UDP_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_UDP_SOCKET_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/native/detail/reactor/reactor_datagram_socket.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>
#include <boost/corosio/native/detail/select/select_op.hpp>
#include <boost/capy/ex/executor_ref.hpp>

namespace boost::corosio::detail {

class select_udp_service;
class select_udp_socket;

/// select datagram base operation.
struct select_datagram_op : reactor_op<select_udp_socket, select_tcp_acceptor>
{
    void operator()() override;
};

/// select send_to operation.
struct select_send_to_op final : reactor_send_to_op<select_datagram_op>
{
    void cancel() noexcept override;
};

/// select recv_from operation.
struct select_recv_from_op final : reactor_recv_from_op<select_datagram_op>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// Datagram socket implementation for select backend.
class select_udp_socket final
    : public reactor_datagram_socket<
          select_udp_socket,
          select_udp_service,
          select_send_to_op,
          select_recv_from_op,
          select_descriptor_state>
{
    friend class select_udp_service;

public:
    explicit select_udp_socket(select_udp_service& svc) noexcept;
    ~select_udp_socket() override;

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

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_UDP_SOCKET_HPP
