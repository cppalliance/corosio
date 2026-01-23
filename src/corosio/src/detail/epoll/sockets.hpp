//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_EPOLL_SOCKETS_HPP
#define BOOST_COROSIO_DETAIL_EPOLL_SOCKETS_HPP

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_EPOLL)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/acceptor.hpp>
#include <boost/corosio/socket.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/io_awaitable.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include "src/detail/intrusive.hpp"

#include "src/detail/epoll/op.hpp"
#include "src/detail/epoll/scheduler.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

/*
    epoll Socket Implementation
    ===========================

    Each I/O operation follows the same pattern:
      1. Try the syscall immediately (non-blocking socket)
      2. If it succeeds or fails with a real error, post to completion queue
      3. If EAGAIN/EWOULDBLOCK, register with epoll and wait

    This "try first" approach avoids unnecessary epoll round-trips for
    operations that can complete immediately (common for small reads/writes
    on fast local connections).

    One-Shot Registration
    ---------------------
    We use one-shot epoll registration: each operation registers, waits for
    one event, then unregisters. This simplifies the state machine since we
    don't need to track whether an fd is currently registered or handle
    re-arming. The tradeoff is slightly more epoll_ctl calls, but the
    simplicity is worth it.

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

    Service Ownership
    -----------------
    epoll_sockets owns all socket impls. destroy_impl() removes the shared_ptr
    from the vector, but the impl may survive if ops still hold impl_ptr refs.
    shutdown() closes all sockets and clears the vectors; any in-flight ops
    will complete and release their refs, allowing final destruction.
*/

namespace boost {
namespace corosio {
namespace detail {

class epoll_sockets;
class epoll_socket_impl;
class epoll_acceptor_impl;

//------------------------------------------------------------------------------

class epoll_socket_impl
    : public socket::socket_impl
    , public std::enable_shared_from_this<epoll_socket_impl>
    , public intrusive_list<epoll_socket_impl>::node
{
    friend class epoll_sockets;

public:
    explicit epoll_socket_impl(epoll_sockets& svc) noexcept;

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
    bool is_open() const noexcept { return fd_ >= 0; }
    void cancel() noexcept;
    void close_socket() noexcept;
    void set_socket(int fd) noexcept { fd_ = fd; }

    epoll_connect_op conn_;
    epoll_read_op rd_;
    epoll_write_op wr_;

private:
    epoll_sockets& svc_;
    int fd_ = -1;
};

//------------------------------------------------------------------------------

class epoll_acceptor_impl
    : public acceptor::acceptor_impl
    , public std::enable_shared_from_this<epoll_acceptor_impl>
    , public intrusive_list<epoll_acceptor_impl>::node
{
    friend class epoll_sockets;

public:
    explicit epoll_acceptor_impl(epoll_sockets& svc) noexcept;

    void release() override;

    void accept(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::stop_token,
        system::error_code*,
        io_object::io_object_impl**) override;

    int native_handle() const noexcept { return fd_; }
    bool is_open() const noexcept { return fd_ >= 0; }
    void cancel() noexcept;
    void close_socket() noexcept;

    epoll_accept_op acc_;

private:
    epoll_sockets& svc_;
    int fd_ = -1;
};

//------------------------------------------------------------------------------

class epoll_sockets
    : public capy::execution_context::service
{
public:
    using key_type = epoll_sockets;

    explicit epoll_sockets(capy::execution_context& ctx);
    ~epoll_sockets();

    epoll_sockets(epoll_sockets const&) = delete;
    epoll_sockets& operator=(epoll_sockets const&) = delete;

    void shutdown() override;

    epoll_socket_impl& create_impl();
    void destroy_impl(epoll_socket_impl& impl);
    system::error_code open_socket(epoll_socket_impl& impl);

    epoll_acceptor_impl& create_acceptor_impl();
    void destroy_acceptor_impl(epoll_acceptor_impl& impl);
    system::error_code open_acceptor(
        epoll_acceptor_impl& impl,
        endpoint ep,
        int backlog);

    epoll_scheduler& scheduler() const noexcept { return sched_; }
    void post(epoll_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

private:
    epoll_scheduler& sched_;
    std::mutex mutex_;

    intrusive_list<epoll_socket_impl> socket_list_;
    intrusive_list<epoll_acceptor_impl> acceptor_list_;
    std::unordered_map<epoll_socket_impl*, std::shared_ptr<epoll_socket_impl>> socket_ptrs_;
    std::unordered_map<epoll_acceptor_impl*, std::shared_ptr<epoll_acceptor_impl>> acceptor_ptrs_;
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // BOOST_COROSIO_BACKEND_EPOLL

#endif // BOOST_COROSIO_DETAIL_EPOLL_SOCKETS_HPP
