//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_SOCKET_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/native/detail/reactor/reactor_socket.hpp>
#include <boost/corosio/native/detail/select/select_op.hpp>
#include <boost/capy/ex/executor_ref.hpp>

namespace boost::corosio::detail {

class select_socket_service;

/// Socket implementation for select backend.
class select_socket final
    : public reactor_socket<
          select_socket,
          select_socket_service,
          select_op,
          select_connect_op,
          select_read_op,
          select_write_op,
          select_descriptor_state>
{
    friend class select_socket_service;

public:
    explicit select_socket(select_socket_service& svc) noexcept;
    ~select_socket() override;

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

    void cancel() noexcept override;
    void close_socket() noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_SOCKET_HPP
