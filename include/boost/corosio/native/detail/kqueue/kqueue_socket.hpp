//
// Copyright (c) 2026 Michael Vandeberg
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_SOCKET_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/native/detail/reactor/reactor_socket.hpp>
#include <boost/corosio/native/detail/kqueue/kqueue_op.hpp>
#include <boost/capy/ex/executor_ref.hpp>

namespace boost::corosio::detail {

class kqueue_socket_service;

/// Socket implementation for kqueue backend.
class kqueue_socket final
    : public reactor_socket<
        kqueue_socket,
        kqueue_socket_service,
        kqueue_op,
        kqueue_connect_op,
        kqueue_read_op,
        kqueue_write_op,
        descriptor_state>
{
    friend class kqueue_socket_service;

    bool user_set_linger_ = false;

public:
    explicit kqueue_socket(kqueue_socket_service& svc) noexcept;
    ~kqueue_socket();

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

    /// Track SO_LINGER for macOS kqueue workaround.
    std::error_code set_option(
        int level,
        int optname,
        void const* data,
        std::size_t size) noexcept override;

    void cancel() noexcept override;
    void close_socket() noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_SOCKET_HPP
