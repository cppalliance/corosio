//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_ACCEPTOR_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_ACCEPTOR_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>

#include <boost/corosio/native/detail/epoll/epoll_op.hpp>

#include <memory>

namespace boost::corosio::detail {

class epoll_acceptor_service;

/// Acceptor implementation for epoll backend.
class epoll_acceptor final
    : public tcp_acceptor::implementation
    , public std::enable_shared_from_this<epoll_acceptor>
    , public intrusive_list<epoll_acceptor>::node
{
    friend class epoll_acceptor_service;

public:
    explicit epoll_acceptor(epoll_acceptor_service& svc) noexcept;

    std::coroutine_handle<> accept(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::stop_token,
        std::error_code*,
        io_object::implementation**) override;

    int native_handle() const noexcept
    {
        return fd_;
    }
    endpoint local_endpoint() const noexcept override
    {
        return local_endpoint_;
    }
    bool is_open() const noexcept override
    {
        return fd_ >= 0;
    }
    void cancel() noexcept override;

    std::error_code set_option(
        int level, int optname,
        void const* data, std::size_t size) noexcept override;
    std::error_code get_option(
        int level, int optname,
        void* data, std::size_t* size) const noexcept override;
    void cancel_single_op(epoll_op& op) noexcept;
    void close_socket() noexcept;
    void set_local_endpoint(endpoint ep) noexcept
    {
        local_endpoint_ = ep;
    }

    epoll_acceptor_service& service() noexcept
    {
        return svc_;
    }

    epoll_accept_op acc_;
    descriptor_state desc_state_;

private:
    epoll_acceptor_service& svc_;
    int fd_ = -1;
    endpoint local_endpoint_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_ACCEPTOR_HPP
