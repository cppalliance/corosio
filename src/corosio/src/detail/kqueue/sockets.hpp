//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_KQUEUE_SOCKETS_HPP
#define BOOST_COROSIO_DETAIL_KQUEUE_SOCKETS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include "src/detail/intrusive.hpp"
#include "src/detail/socket_service.hpp"

#include "src/detail/kqueue/op.hpp"
#include "src/detail/kqueue/scheduler.hpp"

#include <coroutine>
#include <memory>
#include <mutex>
#include <unordered_map>

/*
    kqueue Socket Implementation
    ============================

    Each I/O operation follows the same pattern:
      1. Try the syscall speculatively (readv/writev) before suspending
      2. On success, return via symmetric transfer (the "pump" fast path)
      3. On budget exhaustion, post to the scheduler queue for fairness
      4. On EAGAIN, register_op() parks the op in the descriptor_state

    The speculative path avoids scheduler queue, mutex, and reactor
    round-trips entirely. An inline budget limits consecutive inline
    completions to prevent starvation of other connections.

    Cancellation
    ------------
    See op.hpp for the completion/cancellation race handling via the
    descriptor_state mutex. cancel() must complete pending operations (post
    them with cancelled flag) so coroutines waiting on them can resume.
    close_socket() calls cancel() first to ensure this.

    Impl Lifetime with shared_ptr
    -----------------------------
    Socket impls use enable_shared_from_this. The service owns impls via
    shared_ptr maps (socket_ptrs_) keyed by raw pointer for O(1) lookup and
    removal. When a user calls close(), we call cancel() which posts pending
    ops to the scheduler.

    CRITICAL: The posted ops must keep the impl alive until they complete.
    Otherwise the scheduler would process a freed op (use-after-free). The
    cancel() method captures shared_from_this() into op.impl_ptr before
    posting. When the op completes, impl_ptr is cleared, allowing the impl
    to be destroyed if no other references exist.

    Service Ownership
    -----------------
    kqueue_socket_service owns all socket impls. destroy_impl() removes the
    shared_ptr from the map, but the impl may survive if ops still hold
    impl_ptr refs. shutdown() closes all sockets and clears the map; any
    in-flight ops will complete and release their refs.
*/

namespace boost::corosio::detail {

class kqueue_socket_service;
class kqueue_socket_impl;

/// Socket implementation for kqueue backend.
class kqueue_socket_impl
    : public tcp_socket::socket_impl
    , public std::enable_shared_from_this<kqueue_socket_impl>
    , public intrusive_list<kqueue_socket_impl>::node
{
    friend class kqueue_socket_service;

public:
    explicit kqueue_socket_impl(kqueue_socket_service& svc) noexcept;
    ~kqueue_socket_impl();

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

    native_handle_type native_handle() const noexcept override { return fd_; }

    // Socket options
    std::error_code set_no_delay(bool value) noexcept override;
    bool no_delay(std::error_code& ec) const noexcept override;

    std::error_code set_keep_alive(bool value) noexcept override;
    bool keep_alive(std::error_code& ec) const noexcept override;

    std::error_code set_receive_buffer_size(int size) noexcept override;
    int receive_buffer_size(std::error_code& ec) const noexcept override;

    std::error_code set_send_buffer_size(int size) noexcept override;
    int send_buffer_size(std::error_code& ec) const noexcept override;

    std::error_code set_linger(bool enabled, int timeout) noexcept override;
    tcp_socket::linger_options linger(std::error_code& ec) const noexcept override;

    endpoint local_endpoint() const noexcept override { return local_endpoint_; }
    endpoint remote_endpoint() const noexcept override { return remote_endpoint_; }
    bool is_open() const noexcept { return fd_ >= 0; }
    void cancel() noexcept override;
    void cancel_single_op(kqueue_op& op) noexcept;
    void close_socket() noexcept;
    void set_socket(int fd) noexcept { fd_ = fd; }
    void set_endpoints(endpoint local, endpoint remote) noexcept
    {
        local_endpoint_ = local;
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
    int fd_ = -1;
    endpoint local_endpoint_;
    endpoint remote_endpoint_;
};

/** State for kqueue socket service. */
class kqueue_socket_state
{
public:
    explicit kqueue_socket_state(kqueue_scheduler& sched) noexcept
        : sched_(sched)
    {
    }

    kqueue_scheduler& sched_;
    std::mutex mutex_;
    intrusive_list<kqueue_socket_impl> socket_list_;
    std::unordered_map<kqueue_socket_impl*, std::shared_ptr<kqueue_socket_impl>> socket_ptrs_;
};

/** kqueue socket service implementation.

    Inherits from socket_service to enable runtime polymorphism.
    Uses key_type = socket_service for service lookup.
*/
class kqueue_socket_service : public socket_service
{
public:
    explicit kqueue_socket_service(capy::execution_context& ctx);
    ~kqueue_socket_service();

    kqueue_socket_service(kqueue_socket_service const&) = delete;
    kqueue_socket_service& operator=(kqueue_socket_service const&) = delete;

    void shutdown() override;

    io_object::io_object_impl* construct() override;
    void destroy(io_object::io_object_impl*) override;
    void open(io_object::handle&) override;
    void close(io_object::handle&) override;
    std::error_code open_socket(tcp_socket::socket_impl& impl) override;

    kqueue_scheduler& scheduler() const noexcept { return state_->sched_; }
    void post(kqueue_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

private:
    std::unique_ptr<kqueue_socket_state> state_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_DETAIL_KQUEUE_SOCKETS_HPP
