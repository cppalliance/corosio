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

#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>

#include <boost/corosio/native/detail/kqueue/kqueue_op.hpp>

#include <memory>

namespace boost::corosio::detail {

class kqueue_socket_service;

/// Socket implementation for kqueue backend.
class kqueue_socket final
    : public tcp_socket::implementation
    , public std::enable_shared_from_this<kqueue_socket>
    , public intrusive_list<kqueue_socket>::node
{
    friend class kqueue_socket_service;

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

    std::error_code shutdown(tcp_socket::shutdown_type what) noexcept override;

    native_handle_type native_handle() const noexcept override
    {
        return fd_;
    }

    // Socket options
    std::error_code set_option(
        int level,
        int optname,
        void const* data,
        std::size_t size) noexcept override;
    std::error_code
    get_option(int level, int optname, void* data, std::size_t* size)
        const noexcept override;

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
    void cancel_single_op(kqueue_op& op) noexcept;
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

    // Public for internal integration with the scheduler and reactor —
    // not part of the external API. The descriptor_state is accessed by
    // the reactor thread (lock-free atomics) and by op completion under
    // desc_state_.mutex; the op slots and initiators are only touched
    // by the thread that owns the current I/O call.
    kqueue_connect_op conn_;
    kqueue_read_op rd_;
    kqueue_write_op wr_;
    descriptor_state desc_state_;

    void register_op(
        kqueue_op& op,
        kqueue_op*& desc_slot,
        bool& ready_flag,
        bool& cancel_flag) noexcept;

private:
    kqueue_socket_service& svc_;
    int fd_               = -1;
    bool user_set_linger_ = false;
    endpoint local_endpoint_;
    endpoint remote_endpoint_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_SOCKET_HPP
