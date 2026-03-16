//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_TCP_ACCEPTOR_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_TCP_ACCEPTOR_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/native/detail/reactor/reactor_acceptor.hpp>
#include <boost/corosio/native/detail/select/select_op.hpp>
#include <boost/capy/ex/executor_ref.hpp>

namespace boost::corosio::detail {

class select_tcp_acceptor_service;

/// Acceptor implementation for select backend.
class select_tcp_acceptor final
    : public reactor_acceptor<
          select_tcp_acceptor,
          select_tcp_acceptor_service,
          select_op,
          select_accept_op,
          select_descriptor_state>
{
    friend class select_tcp_acceptor_service;

public:
    explicit select_tcp_acceptor(select_tcp_acceptor_service& svc) noexcept;

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

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_TCP_ACCEPTOR_HPP
