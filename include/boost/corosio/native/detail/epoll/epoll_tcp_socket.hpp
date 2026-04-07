//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_TCP_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_TCP_SOCKET_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/native/detail/reactor/reactor_stream_socket.hpp>
#include <boost/corosio/native/detail/epoll/epoll_op.hpp>
#include <boost/capy/ex/executor_ref.hpp>

namespace boost::corosio::detail {

class epoll_tcp_service;

/// Stream socket implementation for epoll backend.
class epoll_tcp_socket final
    : public reactor_stream_socket<
          epoll_tcp_socket,
          epoll_tcp_service,
          epoll_connect_op,
          epoll_read_op,
          epoll_write_op,
          descriptor_state>
{
    friend class epoll_tcp_service;

public:
    explicit epoll_tcp_socket(epoll_tcp_service& svc) noexcept;
    ~epoll_tcp_socket() override;

    std::coroutine_handle<> connect(
        std::coroutine_handle<>,
        capy::executor_ref,
        endpoint,
        std::stop_token,
        std::error_code*) override;

    std::coroutine_handle<> read_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    std::coroutine_handle<> write_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    std::error_code shutdown(tcp_socket::shutdown_type what) noexcept override
    {
        return do_shutdown(static_cast<int>(what));
    }

    void cancel() noexcept override;
    void close_socket() noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_TCP_SOCKET_HPP
