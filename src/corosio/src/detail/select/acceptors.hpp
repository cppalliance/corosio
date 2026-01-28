//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_SELECT_ACCEPTORS_HPP
#define BOOST_COROSIO_DETAIL_SELECT_ACCEPTORS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/acceptor.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include "src/detail/intrusive.hpp"
#include "src/detail/socket_service.hpp"

#include "src/detail/select/op.hpp"
#include "src/detail/select/scheduler.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace boost::corosio::detail {

class select_acceptor_service;
class select_acceptor_impl;
class select_socket_service;

//------------------------------------------------------------------------------

class select_acceptor_impl
    : public acceptor::acceptor_impl
    , public std::enable_shared_from_this<select_acceptor_impl>
    , public intrusive_list<select_acceptor_impl>::node
{
    friend class select_acceptor_service;

public:
    explicit select_acceptor_impl(select_acceptor_service& svc) noexcept;

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
    void cancel_single_op(select_op& op) noexcept;
    void close_socket() noexcept;
    void set_local_endpoint(endpoint ep) noexcept { local_endpoint_ = ep; }

    select_acceptor_service& service() noexcept { return svc_; }

    select_accept_op acc_;

private:
    select_acceptor_service& svc_;
    int fd_ = -1;
    endpoint local_endpoint_;
};

//------------------------------------------------------------------------------

/** State for select acceptor service. */
class select_acceptor_state
{
public:
    explicit select_acceptor_state(select_scheduler& sched) noexcept
        : sched_(sched)
    {
    }

    select_scheduler& sched_;
    std::mutex mutex_;
    intrusive_list<select_acceptor_impl> acceptor_list_;
    std::unordered_map<select_acceptor_impl*, std::shared_ptr<select_acceptor_impl>> acceptor_ptrs_;
};

/** select acceptor service implementation.

    Inherits from acceptor_service to enable runtime polymorphism.
    Uses key_type = acceptor_service for service lookup.
*/
class select_acceptor_service : public acceptor_service
{
public:
    explicit select_acceptor_service(capy::execution_context& ctx);
    ~select_acceptor_service();

    select_acceptor_service(select_acceptor_service const&) = delete;
    select_acceptor_service& operator=(select_acceptor_service const&) = delete;

    void shutdown() override;

    acceptor::acceptor_impl& create_acceptor_impl() override;
    void destroy_acceptor_impl(acceptor::acceptor_impl& impl) override;
    system::error_code open_acceptor(
        acceptor::acceptor_impl& impl,
        endpoint ep,
        int backlog) override;

    select_scheduler& scheduler() const noexcept { return state_->sched_; }
    void post(select_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

    /** Get the socket service for creating peer sockets during accept. */
    select_socket_service* socket_service() const noexcept;

private:
    capy::execution_context& ctx_;
    std::unique_ptr<select_acceptor_state> state_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_DETAIL_SELECT_ACCEPTORS_HPP
