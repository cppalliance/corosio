//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_LOCAL_STREAM_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_LOCAL_STREAM_SOCKET_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/native/detail/reactor/reactor_stream_socket.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op.hpp>
#include <boost/corosio/native/detail/select/select_op.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/capy/ex/executor_ref.hpp>

namespace boost::corosio::detail {

// Forward declarations
class select_local_stream_service;
class select_local_stream_socket;
class select_local_stream_acceptor;

/// Base operation for local stream sockets on select.
struct select_local_stream_op
    : reactor_op<select_local_stream_socket, select_local_stream_acceptor>
{
    void operator()() override;
};

/// Connect operation for local stream sockets.
struct select_local_connect_op final
    : reactor_connect_op<select_local_stream_op, local_endpoint>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// Scatter-read operation for local stream sockets.
struct select_local_read_op final : reactor_read_op<select_local_stream_op>
{
    void cancel() noexcept override;
};

/// Gather-write operation for local stream sockets.
struct select_local_write_op final
    : reactor_write_op<select_local_stream_op, select_write_policy>
{
    void cancel() noexcept override;
};

/// Accept operation for local stream sockets.
struct select_local_accept_op final
    : reactor_accept_op<select_local_stream_op, select_accept_policy>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// Stream socket implementation for local (Unix) sockets on select.
class select_local_stream_socket final
    : public reactor_stream_socket<
          select_local_stream_socket,
          select_local_stream_service,
          select_local_connect_op,
          select_local_read_op,
          select_local_write_op,
          select_descriptor_state,
          local_stream_socket::implementation,
          corosio::local_endpoint>
{
    friend class select_local_stream_service;

public:
    explicit select_local_stream_socket(
        select_local_stream_service& svc) noexcept;
    ~select_local_stream_socket() override;

    std::coroutine_handle<> connect(
        std::coroutine_handle<>,
        capy::executor_ref,
        corosio::local_endpoint,
        std::stop_token,
        std::error_code*) override;

    std::error_code shutdown(
        local_stream_socket::shutdown_type what) noexcept override
    {
        return do_shutdown(static_cast<int>(what));
    }

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
    native_handle_type release_socket() noexcept override;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_LOCAL_STREAM_SOCKET_HPP
