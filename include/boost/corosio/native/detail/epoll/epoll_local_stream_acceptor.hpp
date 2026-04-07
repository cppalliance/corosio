//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_LOCAL_STREAM_ACCEPTOR_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_LOCAL_STREAM_ACCEPTOR_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/native/detail/reactor/reactor_acceptor.hpp>
#include <boost/corosio/native/detail/epoll/epoll_local_stream_socket.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/capy/ex/executor_ref.hpp>

namespace boost::corosio::detail {

class epoll_local_stream_acceptor_service;

/// Acceptor implementation for local stream sockets on epoll.
class epoll_local_stream_acceptor final
    : public reactor_acceptor<
          epoll_local_stream_acceptor,
          epoll_local_stream_acceptor_service,
          epoll_local_stream_op,
          epoll_local_accept_op,
          descriptor_state,
          local_stream_acceptor::implementation,
          local_endpoint>
{
    friend class epoll_local_stream_acceptor_service;

public:
    explicit epoll_local_stream_acceptor(
        epoll_local_stream_acceptor_service& svc) noexcept;

    std::coroutine_handle<> accept(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::stop_token,
        std::error_code*,
        io_object::implementation**) override;

    void cancel() noexcept override;
    void close_socket() noexcept;
    native_handle_type release_socket() noexcept override;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_LOCAL_STREAM_ACCEPTOR_HPP
