//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_SOCKET_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>

#include <boost/corosio/native/detail/epoll/epoll_op.hpp>

#include <memory>

namespace boost::corosio::detail {

class epoll_socket_service;

/// Socket implementation for epoll backend.
class epoll_socket final
    : public tcp_socket::implementation
    , public std::enable_shared_from_this<epoll_socket>
    , public intrusive_list<epoll_socket>::node
{
    friend class epoll_socket_service;

public:
    explicit epoll_socket(epoll_socket_service& svc) noexcept;
    ~epoll_socket() override;

    std::coroutine_handle<> connect(
        std::coroutine_handle<>,
        capy::executor_ref,
        endpoint,
        std::stop_token,
        std::error_code*) override;

    std::coroutine_handle<> read_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        io_buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    std::coroutine_handle<> write_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        io_buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    std::error_code shutdown(tcp_socket::shutdown_type what) noexcept override;

    native_handle_type native_handle() const noexcept override
    {
        return fd_;
    }

    std::error_code set_option(
        int level, int optname,
        void const* data, std::size_t size) noexcept override;
    std::error_code get_option(
        int level, int optname,
        void* data, std::size_t* size) const noexcept override;

    endpoint local_endpoint() const noexcept override
    {
        return local_endpoint_;
    }
    endpoint remote_endpoint() const noexcept override
    {
        return remote_endpoint_;
    }
    bool is_open() const noexcept
    {
        return fd_ >= 0;
    }
    void cancel() noexcept override;
    void cancel_single_op(epoll_op& op) noexcept;
    void close_socket() noexcept;
    void set_socket(int fd) noexcept
    {
        fd_ = fd;
    }
    void set_endpoints(endpoint local, endpoint remote) noexcept
    {
        local_endpoint_  = local;
        remote_endpoint_ = remote;
    }

    epoll_connect_op conn_;
    epoll_read_op rd_;
    epoll_write_op wr_;

    /// Per-descriptor state for persistent epoll registration
    descriptor_state desc_state_;

private:
    epoll_socket_service& svc_;
    int fd_ = -1;
    endpoint local_endpoint_;
    endpoint remote_endpoint_;

    void register_op(
        epoll_op& op,
        epoll_op*& desc_slot,
        bool& ready_flag,
        bool& cancel_flag) noexcept;

    friend struct epoll_op;
    friend struct epoll_connect_op;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_SOCKET_HPP
