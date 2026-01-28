//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_EPOLL_ACCEPTORS_HPP
#define BOOST_COROSIO_DETAIL_EPOLL_ACCEPTORS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/acceptor.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include "src/detail/intrusive.hpp"
#include "src/detail/socket_service.hpp"

#include "src/detail/epoll/op.hpp"
#include "src/detail/epoll/scheduler.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace boost::corosio::detail {

class epoll_acceptor_service;
class epoll_acceptor_impl;
class epoll_socket_service;

//------------------------------------------------------------------------------

class epoll_acceptor_impl
    : public acceptor::acceptor_impl
    , public std::enable_shared_from_this<epoll_acceptor_impl>
    , public intrusive_list<epoll_acceptor_impl>::node
{
    friend class epoll_acceptor_service;

public:
    explicit epoll_acceptor_impl(epoll_acceptor_service& svc) noexcept;

    void release() override;

    void accept(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::stop_token,
        system::error_code*,
        io_object::io_object_impl**) override;

    int native_handle() const noexcept { return fd_; }
    endpoint local_endpoint() const noexcept override { return local_endpoint_; }
    bool is_open() const noexcept { return fd_ >= 0; }
    void cancel() noexcept override;
    void cancel_single_op(epoll_op& op) noexcept;
    void close_socket() noexcept;
    void set_local_endpoint(endpoint ep) noexcept { local_endpoint_ = ep; }

    epoll_acceptor_service& service() noexcept { return svc_; }

    epoll_accept_op acc_;

private:
    epoll_acceptor_service& svc_;
    int fd_ = -1;
    endpoint local_endpoint_;
};

//------------------------------------------------------------------------------

/** State for epoll acceptor service. */
class epoll_acceptor_state
{
public:
    explicit epoll_acceptor_state(epoll_scheduler& sched) noexcept
        : sched_(sched)
    {
    }

    epoll_scheduler& sched_;
    std::mutex mutex_;
    intrusive_list<epoll_acceptor_impl> acceptor_list_;
    std::unordered_map<epoll_acceptor_impl*, std::shared_ptr<epoll_acceptor_impl>> acceptor_ptrs_;
};

/** epoll acceptor service implementation.

    Inherits from acceptor_service to enable runtime polymorphism.
    Uses key_type = acceptor_service for service lookup.
*/
class epoll_acceptor_service : public acceptor_service
{
public:
    explicit epoll_acceptor_service(capy::execution_context& ctx);
    ~epoll_acceptor_service();

    epoll_acceptor_service(epoll_acceptor_service const&) = delete;
    epoll_acceptor_service& operator=(epoll_acceptor_service const&) = delete;

    void shutdown() override;

    acceptor::acceptor_impl& create_acceptor_impl() override;
    void destroy_acceptor_impl(acceptor::acceptor_impl& impl) override;
    system::error_code open_acceptor(
        acceptor::acceptor_impl& impl,
        endpoint ep,
        int backlog) override;

    epoll_scheduler& scheduler() const noexcept { return state_->sched_; }
    void post(epoll_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

    /** Get the socket service for creating peer sockets during accept. */
    epoll_socket_service* socket_service() const noexcept;

private:
    capy::execution_context& ctx_;
    std::unique_ptr<epoll_acceptor_state> state_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_DETAIL_EPOLL_ACCEPTORS_HPP
