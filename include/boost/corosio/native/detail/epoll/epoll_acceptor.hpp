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

#include <boost/corosio/native/detail/reactor/reactor_acceptor.hpp>
#include <boost/corosio/native/detail/epoll/epoll_op.hpp>
#include <boost/capy/ex/executor_ref.hpp>

namespace boost::corosio::detail {

class epoll_acceptor_service;

/// Acceptor implementation for epoll backend.
class epoll_acceptor final
    : public reactor_acceptor<
        epoll_acceptor,
        epoll_acceptor_service,
        epoll_op,
        epoll_accept_op,
        descriptor_state>
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

    void cancel() noexcept override;
    void close_socket() noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_ACCEPTOR_HPP
