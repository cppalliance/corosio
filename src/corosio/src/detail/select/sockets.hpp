//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_SELECT_SOCKETS_HPP
#define BOOST_COROSIO_DETAIL_SELECT_SOCKETS_HPP


#if !defined(_WIN32)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/acceptor.hpp>
#include <boost/corosio/socket.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include "src/detail/intrusive.hpp"
#include "src/detail/socket_service.hpp"

#include "src/detail/select/op.hpp"
#include "src/detail/select/scheduler.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

/*
    select Socket Implementation
    ============================

    This mirrors the epoll_sockets design for behavioral consistency.
    Each I/O operation follows the same pattern:
      1. Try the syscall immediately (non-blocking socket)
      2. If it succeeds or fails with a real error, post to completion queue
      3. If EAGAIN/EWOULDBLOCK, register with select scheduler and wait

    Cancellation
    ------------
    See op.hpp for the completion/cancellation race handling via the
    `registered` atomic. cancel() must complete pending operations (post
    them with cancelled flag) so coroutines waiting on them can resume.
    close_socket() calls cancel() first to ensure this.

    Impl Lifetime with shared_ptr
    -----------------------------
    Socket and acceptor impls use enable_shared_from_this. The service owns
    impls via shared_ptr maps (socket_ptrs_, acceptor_ptrs_) keyed by raw
    pointer for O(1) lookup and removal. When a user calls close(), we call
    cancel() which posts pending ops to the scheduler.

    CRITICAL: The posted ops must keep the impl alive until they complete.
    Otherwise the scheduler would process a freed op (use-after-free). The
    cancel() method captures shared_from_this() into op.impl_ptr before
    posting. When the op completes, impl_ptr is cleared, allowing the impl
    to be destroyed if no other references exist.

    The intrusive_list (socket_list_, acceptor_list_) provides fast iteration
    for shutdown cleanup alongside the shared_ptr ownership in the maps.

    Service Lookup
    --------------
    Both services inherit from abstract base classes (socket_service,
    acceptor_service) with their respective key_type. This enables runtime
    polymorphism: find_service<socket_service>() returns whichever
    implementation (epoll_socket_service or select_socket_service) was
    installed first. The acceptor service uses this to look up the socket
    service when creating peer sockets during accept.
*/

namespace boost::corosio::detail {

class select_socket_service;
class select_acceptor_service;
class select_socket_impl;
class select_acceptor_impl;

//------------------------------------------------------------------------------

class select_socket_impl
    : public socket::socket_impl
    , public std::enable_shared_from_this<select_socket_impl>
    , public intrusive_list<select_socket_impl>::node
{
    friend class select_socket_service;
    friend class select_acceptor_service;

public:
    explicit select_socket_impl(select_socket_service& svc) noexcept;

    void release() override;

    void connect(
        std::coroutine_handle<>,
        capy::executor_ref,
        endpoint,
        std::stop_token,
        system::error_code*) override;

    void read_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        io_buffer_param,
        std::stop_token,
        system::error_code*,
        std::size_t*) override;

    void write_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        io_buffer_param,
        std::stop_token,
        system::error_code*,
        std::size_t*) override;

    system::error_code shutdown(socket::shutdown_type what) noexcept override;

    native_handle_type native_handle() const noexcept override { return fd_; }

    // Socket options
    system::error_code set_no_delay(bool value) noexcept override;
    bool no_delay(system::error_code& ec) const noexcept override;

    system::error_code set_keep_alive(bool value) noexcept override;
    bool keep_alive(system::error_code& ec) const noexcept override;

    system::error_code set_receive_buffer_size(int size) noexcept override;
    int receive_buffer_size(system::error_code& ec) const noexcept override;

    system::error_code set_send_buffer_size(int size) noexcept override;
    int send_buffer_size(system::error_code& ec) const noexcept override;

    system::error_code set_linger(bool enabled, int timeout) noexcept override;
    socket::linger_options linger(system::error_code& ec) const noexcept override;

    endpoint local_endpoint() const noexcept override { return local_endpoint_; }
    endpoint remote_endpoint() const noexcept override { return remote_endpoint_; }
    bool is_open() const noexcept { return fd_ >= 0; }
    void cancel() noexcept override;
    void cancel_single_op(select_op& op) noexcept;
    void close_socket() noexcept;
    void set_socket(int fd) noexcept { fd_ = fd; }
    void set_endpoints(endpoint local, endpoint remote) noexcept
    {
        local_endpoint_ = local;
        remote_endpoint_ = remote;
    }

    select_connect_op conn_;
    select_read_op rd_;
    select_write_op wr_;

private:
    select_socket_service& svc_;
    int fd_ = -1;
    endpoint local_endpoint_;
    endpoint remote_endpoint_;
};

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
class select_acceptor_service;

/** State for select socket service. */
class select_socket_state
{
public:
    explicit select_socket_state(select_scheduler& sched) noexcept
        : sched_(sched)
    {
    }

    select_scheduler& sched_;
    std::mutex mutex_;
    intrusive_list<select_socket_impl> socket_list_;
    std::unordered_map<select_socket_impl*, std::shared_ptr<select_socket_impl>> socket_ptrs_;
};

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

/** select socket service implementation.

    Inherits from socket_service to enable runtime polymorphism.
    Uses key_type = socket_service for service lookup.
*/
class select_socket_service : public socket_service
{
public:
    explicit select_socket_service(capy::execution_context& ctx);
    ~select_socket_service();

    select_socket_service(select_socket_service const&) = delete;
    select_socket_service& operator=(select_socket_service const&) = delete;

    void shutdown() override;

    socket::socket_impl& create_impl() override;
    void destroy_impl(socket::socket_impl& impl) override;
    system::error_code open_socket(socket::socket_impl& impl) override;

    select_scheduler& scheduler() const noexcept { return state_->sched_; }
    void post(select_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

private:
    std::unique_ptr<select_socket_state> state_;
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

// Backward compatibility alias
using select_sockets = select_socket_service;

} // namespace boost::corosio::detail

#endif // !defined(_WIN32)

#endif // BOOST_COROSIO_DETAIL_SELECT_SOCKETS_HPP
