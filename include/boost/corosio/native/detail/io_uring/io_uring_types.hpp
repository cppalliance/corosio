//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_TYPES_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_TYPES_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IO_URING

#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_acceptor_ops.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_buffer.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_dgram_ops.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_op.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_scheduler.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_multishot_acceptor.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_socket_ops.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/msg_flags.hpp>
#include <boost/corosio/detail/local_datagram_service.hpp>
#include <boost/corosio/detail/local_stream_acceptor_service.hpp>
#include <boost/corosio/detail/local_stream_service.hpp>
#include <boost/corosio/detail/tcp_acceptor_service.hpp>
#include <boost/corosio/detail/tcp_service.hpp>
#include <boost/corosio/detail/udp_service.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/udp_socket.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace boost::corosio::detail {

class io_uring_tcp_service;
class io_uring_tcp_acceptor_service;  // Task 18
class io_uring_local_stream_service;
class io_uring_local_stream_acceptor_service;
class io_uring_udp_service;
class io_uring_local_datagram_service;

/** TCP socket implementation for io_uring.

    Implements `tcp_socket::implementation` using a proactor model:
    read, write, and connect operations are submitted to the kernel
    via `io_uring_submit_op` and complete through the ring's CQE path.

    The object is always owned by a `shared_ptr` managed by the service.
    In-flight ops hold an additional `shared_ptr` copy (`impl_ptr`) so
    the kernel's user-data pointer remains valid until the CQE arrives.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Unsafe. A socket must not have two operations of
    the same type in flight simultaneously.
*/
class BOOST_COROSIO_DECL io_uring_tcp_socket final
    : public tcp_socket::implementation
    , public std::enable_shared_from_this<io_uring_tcp_socket>
{
    friend io_uring_tcp_service;

    int                   fd_     = -1;
    int                   family_ = AF_UNSPEC;  // cached at open_socket
    io_uring_scheduler*   sched_  = nullptr;
    io_uring_tcp_service* svc_    = nullptr;

    endpoint local_endpoint_;
    endpoint remote_endpoint_;

    // Per-fd op slots — embedded to eliminate per-call heap allocation.
    // Single-pending invariant per slot: at most one read, write, or
    // connect in flight on this socket at any time (the awaitable
    // contract).
    uring_read_op    rd_;
    uring_write_op   wr_;
    uring_connect_op conn_;

    mutable detail::speculative_state spec_;

public:
    /** Construct with service and scheduler references.

        Both refs must outlive this socket.  `sched_` and `svc_` are
        intentionally separate so service subclasses can pass a
        different scheduler if needed.

        @param svc   The owning service (Task 13).
        @param sched The io_uring scheduler owned by the context.
    */
    explicit io_uring_tcp_socket(
        io_uring_tcp_service& svc,
        io_uring_scheduler&   sched) noexcept
        : sched_(&sched)
        , svc_(&svc)
    {}

    ~io_uring_tcp_socket() override
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

    // ----------------------------------------------------------------
    // io_stream::implementation
    // ----------------------------------------------------------------

    std::coroutine_handle<> read_some(
        std::coroutine_handle<> h,
        capy::executor_ref      ex,
        buffer_param            buffers,
        std::stop_token         token,
        std::error_code*        ec,
        std::size_t*            bytes) override
    {
        iovec iovecs[io_uring_max_iov];
        int   iovec_count = static_cast<int>(
            buffers.copy_to(
                reinterpret_cast<capy::mutable_buffer*>(iovecs),
                io_uring_max_iov));
        bool stop_now  = token.stop_possible() && token.stop_requested();
        bool empty_buf = (iovec_count == 0);

        ssize_t n             = 0;
        int     err           = 0;
        bool    have_sync_res = stop_now || empty_buf;
        if (!have_sync_res && spec_.may_speculate_read())
        {
            do { n = ::readv(fd_, iovecs, iovec_count); }
            while (n < 0 && errno == EINTR);
            if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            {
                have_sync_res = true;
                if (n < 0) err = errno;
            }
            else
            {
                spec_.on_read_exhausted();
            }
        }

        if (have_sync_res)
        {
            if (sched_->try_consume_inline_budget())
            {
                if (ec)
                {
                    if (stop_now)
                        *ec = capy::error::canceled;
                    else if (err)
                        *ec = make_err(err);
                    else if (n == 0 && !empty_buf)
                        *ec = capy::error::eof;
                    else
                        *ec = {};
                }
                if (bytes)
                    *bytes = (n < 0) ? 0u : static_cast<std::size_t>(n);
                rd_.cont_op.cont.h = h;
                return dispatch_coro(ex, rd_.cont_op.cont);
            }
            rd_.prepare(h, ex, ec, bytes, fd_, sched_,
                shared_from_this(), &spec_, buffers, token);
            if (stop_now)
                rd_.cancelled.store(true, std::memory_order_release);
            else
                rd_.res = (n < 0) ? -err : static_cast<int>(n);
            sched_->work_started();
            {
                io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
                sched_->push_completed_locked(&rd_);
            }
            return std::noop_coroutine();
        }

        rd_.prepare(h, ex, ec, bytes, fd_, sched_,
            shared_from_this(), &spec_, buffers, token);
        sched_->work_started();
        if (rd_.cancelled.load(std::memory_order_acquire))
        {
            io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
            sched_->push_completed_locked(&rd_);
            return std::noop_coroutine();
        }
        io_uring_submit_op(*sched_, &rd_);
        return std::noop_coroutine();
    }

    std::coroutine_handle<> write_some(
        std::coroutine_handle<> h,
        capy::executor_ref      ex,
        buffer_param            buffers,
        std::stop_token         token,
        std::error_code*        ec,
        std::size_t*            bytes) override
    {
        iovec iovecs[io_uring_max_iov];
        int   iovec_count = static_cast<int>(
            buffers.copy_to(
                reinterpret_cast<capy::mutable_buffer*>(iovecs),
                io_uring_max_iov));
        bool stop_now  = token.stop_possible() && token.stop_requested();
        bool empty_buf = (iovec_count == 0);

        ssize_t n             = 0;
        int     err           = 0;
        bool    have_sync_res = stop_now || empty_buf;
        if (!have_sync_res && spec_.may_speculate_write())
        {
            msghdr msg{};
            msg.msg_iov    = iovecs;
            msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(iovec_count);
            do { n = ::sendmsg(fd_, &msg, MSG_NOSIGNAL); }
            while (n < 0 && errno == EINTR);
            if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            {
                have_sync_res = true;
                if (n < 0) err = errno;
            }
            else
            {
                spec_.on_write_exhausted();
            }
        }

        if (have_sync_res)
        {
            if (sched_->try_consume_inline_budget())
            {
                if (ec)
                    *ec = stop_now ? capy::error::canceled
                          : err   ? make_err(err)
                                  : std::error_code{};
                if (bytes)
                    *bytes = (n < 0) ? 0u : static_cast<std::size_t>(n);
                wr_.cont_op.cont.h = h;
                return dispatch_coro(ex, wr_.cont_op.cont);
            }
            wr_.prepare(h, ex, ec, bytes, fd_, sched_,
                shared_from_this(), &spec_, buffers, token);
            if (stop_now)
                wr_.cancelled.store(true, std::memory_order_release);
            else
                wr_.res = (n < 0) ? -err : static_cast<int>(n);
            sched_->work_started();
            {
                io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
                sched_->push_completed_locked(&wr_);
            }
            return std::noop_coroutine();
        }

        wr_.prepare(h, ex, ec, bytes, fd_, sched_,
            shared_from_this(), &spec_, buffers, token);
        sched_->work_started();
        if (wr_.cancelled.load(std::memory_order_acquire))
        {
            io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
            sched_->push_completed_locked(&wr_);
            return std::noop_coroutine();
        }
        io_uring_submit_op(*sched_, &wr_);
        return std::noop_coroutine();
    }

    // ----------------------------------------------------------------
    // tcp_socket::implementation
    // ----------------------------------------------------------------

    std::coroutine_handle<> connect(
        std::coroutine_handle<> h,
        capy::executor_ref      ex,
        endpoint                ep,
        std::stop_token         token,
        std::error_code*        ec) override
    {
        bool stop_now = token.stop_possible() && token.stop_requested();
        if (stop_now)
        {
            if (sched_->try_consume_inline_budget())
            {
                if (ec) *ec = capy::error::canceled;
                conn_.cont_op.cont.h = h;
                return dispatch_coro(ex, conn_.cont_op.cont);
            }
            conn_.addrlen = to_sockaddr(ep, family_, conn_.addr);
            conn_.prepare(h, ex, ec, fd_, sched_, shared_from_this(),
                ep, &remote_endpoint_, &local_endpoint_, token);
            conn_.cancelled.store(true, std::memory_order_release);
            sched_->work_started();
            {
                io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
                sched_->push_completed_locked(&conn_);
            }
            return std::noop_coroutine();
        }

        // A speculative ::connect would leave the fd in EINPROGRESS and
        // a subsequent IORING_OP_CONNECT would see EALREADY — avoid.
        conn_.addrlen = to_sockaddr(ep, family_, conn_.addr);
        conn_.prepare(h, ex, ec, fd_, sched_, shared_from_this(),
            ep, &remote_endpoint_, &local_endpoint_, token);
        sched_->work_started();
        if (conn_.cancelled.load(std::memory_order_acquire))
        {
            io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
            sched_->push_completed_locked(&conn_);
            return std::noop_coroutine();
        }
        io_uring_submit_op(*sched_, &conn_);
        return std::noop_coroutine();
    }

    std::error_code shutdown(tcp_socket::shutdown_type what) noexcept override
    {
        if (::shutdown(fd_, static_cast<int>(what)) != 0)
            return make_err(errno);
        return {};
    }

    native_handle_type native_handle() const noexcept override
    {
        return fd_;
    }

    void cancel() noexcept override
    {
        if (fd_ >= 0)
            sched_->submit_cancel_by_fd(fd_);
    }

    std::error_code set_option(
        int         level,
        int         optname,
        void const* data,
        std::size_t size) noexcept override
    {
        if (::setsockopt(
                fd_, level, optname,
                reinterpret_cast<char const*>(data),
                static_cast<socklen_t>(size)) != 0)
            return make_err(errno);
        return {};
    }

    std::error_code get_option(
        int         level,
        int         optname,
        void*       data,
        std::size_t* size) const noexcept override
    {
        socklen_t len = static_cast<socklen_t>(*size);
        if (::getsockopt(fd_, level, optname,
                reinterpret_cast<char*>(data), &len) != 0)
            return make_err(errno);
        *size = static_cast<std::size_t>(len);
        return {};
    }

    endpoint local_endpoint() const noexcept override
    {
        return local_endpoint_;
    }

    endpoint remote_endpoint() const noexcept override
    {
        return remote_endpoint_;
    }
};

/** TCP socket service for io_uring.

    Owns all `io_uring_tcp_socket` implementations for an `io_context`.
    Satisfies the `tcp_service` interface so the generic `tcp_socket`
    front-end can call `open_socket` and `bind_socket` transparently.

    Socket impls are reference-counted inside the service map; raw
    pointers returned from `construct()` remain valid until `destroy()`
    or `shutdown()` is called.

    @par Thread Safety
    All public member functions are thread-safe.
*/
class BOOST_COROSIO_DECL io_uring_tcp_service final
    : public tcp_service
{
public:
    /// Identifies this service for `execution_context` lookup.
    using key_type = tcp_service;

    /** Construct the TCP service.

        @param ctx The owning execution context. The io_uring scheduler
            must already be registered.
    */
    explicit io_uring_tcp_service(capy::execution_context& ctx)
        : sched_(&ctx.use_service<io_uring_scheduler>())
    {}

    void shutdown() override
    {
        std::vector<std::shared_ptr<io_uring_tcp_socket>> live;
        {
            std::lock_guard lk(mutex_);
            live.reserve(impls_.size());
            for (auto& [_, p] : impls_)
                live.push_back(p);
        }
        // Cancel without the lock held to avoid inversion if cancel()
        // ever needs to re-enter the service.
        for (auto& p : live)
            p->cancel();
    }

    io_object::implementation* construct() override
    {
        auto p   = std::make_shared<io_uring_tcp_socket>(*this, *sched_);
        auto* raw = p.get();
        std::lock_guard lk(mutex_);
        impls_.emplace(raw, std::move(p));
        return raw;
    }

    void destroy(io_object::implementation* p) override
    {
        if (!p)
            return;
        std::lock_guard lk(mutex_);
        impls_.erase(static_cast<io_uring_tcp_socket*>(p));
    }

    // Close the fd eagerly when tcp_socket::close() is called, before
    // destroy() drops the shared_ptr and the destructor runs.
    void close(io_object::handle& h) override
    {
        auto* sock = static_cast<io_uring_tcp_socket*>(h.get());
        if (sock && sock->fd_ >= 0)
        {
            // Cancel pending SQEs before closing. The cancel SQE must
            // be submitted to the kernel while the fd is still open;
            // otherwise IORING_ASYNC_CANCEL_FD resolves to the wrong
            // file if the fd number is immediately recycled.
            sched_->cancel_and_flush(sock->fd_);
            ::close(sock->fd_);
            sock->fd_              = -1;
            sock->local_endpoint_  = endpoint{};
            sock->remote_endpoint_ = endpoint{};
        }
    }

    /** Open a socket fd and associate it with an impl.

        Creates a non-blocking, close-on-exec socket via `socket(2)`.

        @param impl   The socket implementation to initialise.
        @param family Address family (e.g. `AF_INET`, `AF_INET6`).
        @param type   Socket type (e.g. `SOCK_STREAM`).
        @param protocol Protocol number (e.g. `IPPROTO_TCP`).
        @return Error code on failure, empty on success.
    */
    std::error_code open_socket(
        tcp_socket::implementation& impl,
        int family, int type, int protocol) override
    {
        auto& sock = static_cast<io_uring_tcp_socket&>(impl);
        int fd = ::socket(
            family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
        if (fd < 0)
            return make_err(errno);
        if (sock.fd_ >= 0)
        {
            sched_->submit_cancel_by_fd(sock.fd_);
            ::close(sock.fd_);
        }
        sock.fd_     = fd;
        sock.family_ = family;
        // Mirror epoll/select: IPv6 sockets default to v6-only so they
        // behave consistently across platforms regardless of the kernel
        // default for /proc/sys/net/ipv6/bindv6only.
        if (family == AF_INET6)
        {
            int one = 1;
            ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
        }
        return {};
    }

    /** Bind the socket and capture the local endpoint via `getsockname`.

        @param impl The socket implementation to bind.
        @param ep   The local endpoint to bind to.
        @return Error code on failure, empty on success.
    */
    std::error_code bind_socket(
        tcp_socket::implementation& impl, endpoint ep) override
    {
        auto& sock = static_cast<io_uring_tcp_socket&>(impl);
        sockaddr_storage addr{};
        socklen_t len = endpoint_to_sockaddr(ep, addr);
        if (::bind(
                sock.fd_,
                reinterpret_cast<sockaddr*>(&addr), len) < 0)
            return make_err(errno);

        sockaddr_storage local{};
        socklen_t local_len = sizeof(local);
        if (::getsockname(
                sock.fd_,
                reinterpret_cast<sockaddr*>(&local), &local_len) == 0)
            sock.local_endpoint_ = sockaddr_to_endpoint(local);
        return {};
    }

    /** Wrap an already-accepted fd as a new socket impl.

        Called by the acceptor service (Task 17) after `accept(2)`
        returns a connected fd. Captures both endpoints via the provided
        peer address and a `getsockname` call.

        @param fd   Accepted file descriptor (must be non-blocking).
        @param peer Peer endpoint from `accept(2)`.
        @return Raw pointer to the registered impl.
    */
    io_uring_tcp_socket* adopt_fd(int fd, endpoint const& peer)
    {
        auto p = std::make_shared<io_uring_tcp_socket>(*this, *sched_);
        p->fd_              = fd;
        p->remote_endpoint_ = peer;

        sockaddr_storage local{};
        socklen_t len = sizeof(local);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) == 0)
            p->local_endpoint_ = sockaddr_to_endpoint(local);

        std::lock_guard lk(mutex_);
        auto* raw = p.get();
        impls_.emplace(raw, std::move(p));
        return raw;
    }

    /// Return the scheduler used by sockets created by this service.
    io_uring_scheduler& scheduler() noexcept { return *sched_; }

private:
    io_uring_scheduler*  sched_;
    std::mutex           mutex_;
    std::unordered_map<io_uring_tcp_socket*,
                       std::shared_ptr<io_uring_tcp_socket>> impls_;
};

/** TCP acceptor implementation for io_uring.

    Inherits the multishot machinery (parked-fd queue, waiter queue,
    CQE drain on destruction) from `io_uring_multishot_acceptor_base`.
    This class adds only the `accept()` override (matching
    `tcp_acceptor::implementation`'s exact signature) and the
    `adopt_thunk` static that wraps an accepted fd via
    `io_uring_tcp_service::adopt_fd`.
*/
class BOOST_COROSIO_DECL io_uring_tcp_acceptor final
    : public io_uring_multishot_acceptor_base<
          io_uring_tcp_acceptor,
          tcp_acceptor::implementation,
          endpoint,
          io_uring_tcp_service>
{
    friend io_uring_tcp_acceptor_service;

    using base_type = io_uring_multishot_acceptor_base<
        io_uring_tcp_acceptor,
        tcp_acceptor::implementation,
        endpoint,
        io_uring_tcp_service>;

public:
    explicit io_uring_tcp_acceptor(
        io_uring_tcp_acceptor_service&,
        io_uring_scheduler&   sched,
        io_uring_tcp_service& peer_svc) noexcept
        : base_type(sched, peer_svc)
    {}

    std::coroutine_handle<> accept(
        std::coroutine_handle<>     h,
        capy::executor_ref          ex,
        std::stop_token             token,
        std::error_code*            ec,
        io_object::implementation** impl_out) override
    {
        base_type::dispatch_or_queue(h, ex, std::move(token), ec, impl_out);
        return std::noop_coroutine();
    }

    static io_object::implementation* adopt_thunk(
        void* peer_service, int fd,
        sockaddr_storage const& peer, socklen_t /*peer_len*/) noexcept
    {
        auto* svc = static_cast<io_uring_tcp_service*>(peer_service);
        return svc->adopt_fd(fd, sockaddr_to_endpoint(peer));
    }
};

/** TCP acceptor service for io_uring.

    Owns all `io_uring_tcp_acceptor` implementations for an `io_context`.
    Satisfies the `tcp_acceptor_service` interface so the generic
    `tcp_acceptor` front-end can call `open_acceptor_socket`,
    `bind_acceptor`, and `listen_acceptor` transparently.

    Acceptor impls are reference-counted inside the service map; raw
    pointers returned from `construct()` remain valid until `destroy()`
    or `shutdown()` is called.

    @par Thread Safety
    All public member functions are thread-safe.
*/
class BOOST_COROSIO_DECL io_uring_tcp_acceptor_service final
    : public tcp_acceptor_service
{
public:
    /// Identifies this service for `execution_context` lookup.
    using key_type = tcp_acceptor_service;

    /** Construct the TCP acceptor service.

        @param ctx The owning execution context. Both the io_uring scheduler
            and the TCP socket service must already be registered.
    */
    explicit io_uring_tcp_acceptor_service(capy::execution_context& ctx)
        : sched_(&ctx.use_service<io_uring_scheduler>())
        , peer_svc_(&ctx.use_service<io_uring_tcp_service>())
    {}

    void shutdown() override
    {
        std::vector<std::shared_ptr<io_uring_tcp_acceptor>> live;
        {
            std::lock_guard lk(mutex_);
            live.reserve(impls_.size());
            for (auto& [_, p] : impls_)
                live.push_back(p);
        }
        // Cancel without the lock held to avoid inversion if cancel()
        // re-enters the service.
        for (auto& p : live)
            p->cancel();
    }

    io_object::implementation* construct() override
    {
        auto p   = std::make_shared<io_uring_tcp_acceptor>(
            *this, *sched_, *peer_svc_);
        auto* raw = p.get();
        std::lock_guard lk(mutex_);
        impls_.emplace(raw, std::move(p));
        return raw;
    }

    void destroy(io_object::implementation* p) override
    {
        if (!p)
            return;
        std::lock_guard lk(mutex_);
        impls_.erase(static_cast<io_uring_tcp_acceptor*>(p));
    }

    // Close the fd eagerly when tcp_acceptor::close() is called, before
    // destroy() drops the shared_ptr and the destructor runs.
    void close(io_object::handle& h) override
    {
        auto* acc = static_cast<io_uring_tcp_acceptor*>(h.get());
        if (acc && acc->fd_ >= 0)
        {
            // Flush the cancel SQE before closing the fd so the kernel
            // resolves the file from the fd number while it is still
            // valid. drain_waiters_only avoids submitting cancel-by-fd
            // a second time (cancel_and_flush already did it).
            sched_->cancel_and_flush(acc->fd_);
            acc->drain_waiters_only();
            ::close(acc->fd_);
            acc->fd_ = -1;

            // Break the multi_op_ -> impl_ptr (shared_ptr<this>) cycle
            // start_multishot established. The acceptor destructor's
            // drain_cqes_for(multi_op_.get()) is the safety net; here
            // we just drop the cycle so the impl can be released when
            // the user's last shared_ptr does.
            if (acc->multi_op_)
                acc->multi_op_->impl_ptr.reset();
        }
    }

    /** Create a non-blocking, close-on-exec socket for accepting.

        @param impl   The acceptor implementation to initialise.
        @param family Address family (e.g. `AF_INET`, `AF_INET6`).
        @param type   Socket type (e.g. `SOCK_STREAM`).
        @param protocol Protocol number (e.g. `IPPROTO_TCP`).
        @return Error code on failure, empty on success.
    */
    std::error_code open_acceptor_socket(
        tcp_acceptor::implementation& impl,
        int family,
        int type,
        int protocol) override
    {
        auto& acc = static_cast<io_uring_tcp_acceptor&>(impl);
        int fd = ::socket(
            family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
        if (fd < 0)
            return make_err(errno);
        if (acc.fd_ >= 0)
        {
            sched_->submit_cancel_by_fd(acc.fd_);
            ::close(acc.fd_);
        }
        acc.fd_ = fd;
        // Match epoll/select: IPv6 acceptors default to dual-stack
        // (v6-only=false) so they accept both IPv4 and IPv6 connections.
        if (family == AF_INET6)
        {
            int zero = 0;
            ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        }
        return {};
    }

    /** Bind an open acceptor and capture the local endpoint.

        @param impl The acceptor implementation to bind.
        @param ep   The local endpoint to bind to.
        @return Error code on failure, empty on success.
    */
    std::error_code bind_acceptor(
        tcp_acceptor::implementation& impl, endpoint ep) override
    {
        auto& acc = static_cast<io_uring_tcp_acceptor&>(impl);
        sockaddr_storage addr{};
        socklen_t len = endpoint_to_sockaddr(ep, addr);
        if (::bind(
                acc.fd_,
                reinterpret_cast<sockaddr*>(&addr), len) < 0)
            return make_err(errno);

        sockaddr_storage local{};
        socklen_t local_len = sizeof(local);
        if (::getsockname(
                acc.fd_,
                reinterpret_cast<sockaddr*>(&local), &local_len) == 0)
            acc.local_endpoint_ = sockaddr_to_endpoint(local);
        return {};
    }

    /** Start listening and submit the multishot accept SQE.

        Calls `::listen(2)` then arms the io_uring multishot accept
        operation that delivers one CQE per accepted connection.

        @param impl    The acceptor implementation to listen on.
        @param backlog Maximum pending-connection queue length.
        @return Error code on failure, empty on success.
    */
    std::error_code listen_acceptor(
        tcp_acceptor::implementation& impl, int backlog) override
    {
        auto& acc = static_cast<io_uring_tcp_acceptor&>(impl);
        if (::listen(acc.fd_, backlog) < 0)
            return make_err(errno);
        acc.start_multishot();
        return {};
    }

    /// Return the scheduler used by acceptors created by this service.
    io_uring_scheduler& scheduler() noexcept { return *sched_; }

private:
    io_uring_scheduler*   sched_;
    io_uring_tcp_service* peer_svc_;
    std::mutex            mutex_;
    std::unordered_map<io_uring_tcp_acceptor*,
                       std::shared_ptr<io_uring_tcp_acceptor>> impls_;
};

/** Unix domain stream socket implementation for io_uring.

    Implements `local_stream_socket::implementation` using a proactor
    model: read, write, and connect operations are submitted to the
    kernel via `io_uring_submit_op` and complete through the ring's
    CQE path.

    The object is always owned by a `shared_ptr` managed by the service.
    In-flight ops hold an additional `shared_ptr` copy (`impl_ptr`) so
    the kernel's user-data pointer remains valid until the CQE arrives.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Unsafe. A socket must not have two operations of
    the same type in flight simultaneously.
*/
class BOOST_COROSIO_DECL io_uring_local_stream_socket final
    : public local_stream_socket::implementation
    , public std::enable_shared_from_this<io_uring_local_stream_socket>
{
    friend io_uring_local_stream_service;

    int                           fd_    = -1;
    io_uring_scheduler*           sched_ = nullptr;
    io_uring_local_stream_service* svc_  = nullptr;

    corosio::local_endpoint local_endpoint_;
    corosio::local_endpoint remote_endpoint_;

    // Per-fd op slots — embedded to eliminate per-call heap allocation.
    // Single-pending invariant per slot.
    uring_read_op          rd_;
    uring_write_op         wr_;
    uring_local_connect_op conn_;

    mutable detail::speculative_state spec_;

public:
    /** Construct with service and scheduler references.

        Both refs must outlive this socket.

        @param svc   The owning service.
        @param sched The io_uring scheduler owned by the context.
    */
    explicit io_uring_local_stream_socket(
        io_uring_local_stream_service& svc,
        io_uring_scheduler&            sched) noexcept
        : sched_(&sched)
        , svc_(&svc)
    {}

    ~io_uring_local_stream_socket() override
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

    // ----------------------------------------------------------------
    // io_stream::implementation
    // ----------------------------------------------------------------

    std::coroutine_handle<> read_some(
        std::coroutine_handle<> h,
        capy::executor_ref      ex,
        buffer_param            buffers,
        std::stop_token         token,
        std::error_code*        ec,
        std::size_t*            bytes) override
    {
        iovec iovecs[io_uring_max_iov];
        int   iovec_count = static_cast<int>(
            buffers.copy_to(
                reinterpret_cast<capy::mutable_buffer*>(iovecs),
                io_uring_max_iov));
        bool stop_now  = token.stop_possible() && token.stop_requested();
        bool empty_buf = (iovec_count == 0);

        ssize_t n             = 0;
        int     err           = 0;
        bool    have_sync_res = stop_now || empty_buf;
        if (!have_sync_res && spec_.may_speculate_read())
        {
            do { n = ::readv(fd_, iovecs, iovec_count); }
            while (n < 0 && errno == EINTR);
            if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            {
                have_sync_res = true;
                if (n < 0) err = errno;
            }
            else
            {
                spec_.on_read_exhausted();
            }
        }

        if (have_sync_res)
        {
            if (sched_->try_consume_inline_budget())
            {
                if (ec)
                {
                    if (stop_now)
                        *ec = capy::error::canceled;
                    else if (err)
                        *ec = make_err(err);
                    else if (n == 0 && !empty_buf)
                        *ec = capy::error::eof;
                    else
                        *ec = {};
                }
                if (bytes)
                    *bytes = (n < 0) ? 0u : static_cast<std::size_t>(n);
                rd_.cont_op.cont.h = h;
                return dispatch_coro(ex, rd_.cont_op.cont);
            }
            rd_.prepare(h, ex, ec, bytes, fd_, sched_,
                shared_from_this(), &spec_, buffers, token);
            if (stop_now)
                rd_.cancelled.store(true, std::memory_order_release);
            else
                rd_.res = (n < 0) ? -err : static_cast<int>(n);
            sched_->work_started();
            {
                io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
                sched_->push_completed_locked(&rd_);
            }
            return std::noop_coroutine();
        }

        rd_.prepare(h, ex, ec, bytes, fd_, sched_,
            shared_from_this(), &spec_, buffers, token);
        sched_->work_started();
        if (rd_.cancelled.load(std::memory_order_acquire))
        {
            io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
            sched_->push_completed_locked(&rd_);
            return std::noop_coroutine();
        }
        io_uring_submit_op(*sched_, &rd_);
        return std::noop_coroutine();
    }

    std::coroutine_handle<> write_some(
        std::coroutine_handle<> h,
        capy::executor_ref      ex,
        buffer_param            buffers,
        std::stop_token         token,
        std::error_code*        ec,
        std::size_t*            bytes) override
    {
        iovec iovecs[io_uring_max_iov];
        int   iovec_count = static_cast<int>(
            buffers.copy_to(
                reinterpret_cast<capy::mutable_buffer*>(iovecs),
                io_uring_max_iov));
        bool stop_now  = token.stop_possible() && token.stop_requested();
        bool empty_buf = (iovec_count == 0);

        ssize_t n             = 0;
        int     err           = 0;
        bool    have_sync_res = stop_now || empty_buf;
        if (!have_sync_res && spec_.may_speculate_write())
        {
            msghdr msg{};
            msg.msg_iov    = iovecs;
            msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(iovec_count);
            do { n = ::sendmsg(fd_, &msg, MSG_NOSIGNAL); }
            while (n < 0 && errno == EINTR);
            if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            {
                have_sync_res = true;
                if (n < 0) err = errno;
            }
            else
            {
                spec_.on_write_exhausted();
            }
        }

        if (have_sync_res)
        {
            if (sched_->try_consume_inline_budget())
            {
                if (ec)
                    *ec = stop_now ? capy::error::canceled
                          : err   ? make_err(err)
                                  : std::error_code{};
                if (bytes)
                    *bytes = (n < 0) ? 0u : static_cast<std::size_t>(n);
                wr_.cont_op.cont.h = h;
                return dispatch_coro(ex, wr_.cont_op.cont);
            }
            wr_.prepare(h, ex, ec, bytes, fd_, sched_,
                shared_from_this(), &spec_, buffers, token);
            if (stop_now)
                wr_.cancelled.store(true, std::memory_order_release);
            else
                wr_.res = (n < 0) ? -err : static_cast<int>(n);
            sched_->work_started();
            {
                io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
                sched_->push_completed_locked(&wr_);
            }
            return std::noop_coroutine();
        }

        wr_.prepare(h, ex, ec, bytes, fd_, sched_,
            shared_from_this(), &spec_, buffers, token);
        sched_->work_started();
        if (wr_.cancelled.load(std::memory_order_acquire))
        {
            io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
            sched_->push_completed_locked(&wr_);
            return std::noop_coroutine();
        }
        io_uring_submit_op(*sched_, &wr_);
        return std::noop_coroutine();
    }

    // ----------------------------------------------------------------
    // local_stream_socket::implementation
    // ----------------------------------------------------------------

    std::coroutine_handle<> connect(
        std::coroutine_handle<>  h,
        capy::executor_ref       ex,
        corosio::local_endpoint  ep,
        std::stop_token          token,
        std::error_code*         ec) override
    {
        bool stop_now = token.stop_possible() && token.stop_requested();
        if (stop_now)
        {
            if (sched_->try_consume_inline_budget())
            {
                if (ec) *ec = capy::error::canceled;
                conn_.cont_op.cont.h = h;
                return dispatch_coro(ex, conn_.cont_op.cont);
            }
            conn_.addrlen = to_sockaddr(ep, conn_.addr);
            conn_.prepare(h, ex, ec, fd_, sched_, shared_from_this(),
                ep, &remote_endpoint_, &local_endpoint_, token);
            conn_.cancelled.store(true, std::memory_order_release);
            sched_->work_started();
            {
                io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
                sched_->push_completed_locked(&conn_);
            }
            return std::noop_coroutine();
        }

        // A speculative ::connect would leave the fd in EINPROGRESS and
        // a subsequent IORING_OP_CONNECT would see EALREADY — avoid.
        conn_.addrlen = to_sockaddr(ep, conn_.addr);
        conn_.prepare(h, ex, ec, fd_, sched_, shared_from_this(),
            ep, &remote_endpoint_, &local_endpoint_, token);
        sched_->work_started();
        if (conn_.cancelled.load(std::memory_order_acquire))
        {
            io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
            sched_->push_completed_locked(&conn_);
            return std::noop_coroutine();
        }
        io_uring_submit_op(*sched_, &conn_);
        return std::noop_coroutine();
    }

    std::error_code shutdown(local_stream_socket::shutdown_type what) noexcept override
    {
        if (::shutdown(fd_, static_cast<int>(what)) != 0)
            return make_err(errno);
        return {};
    }

    native_handle_type native_handle() const noexcept override
    {
        return fd_;
    }

    native_handle_type release_socket() noexcept override
    {
        int fd = fd_;
        fd_ = -1;
        local_endpoint_  = corosio::local_endpoint{};
        remote_endpoint_ = corosio::local_endpoint{};
        return fd;
    }

    void cancel() noexcept override
    {
        if (fd_ >= 0)
            sched_->submit_cancel_by_fd(fd_);
    }

    std::error_code set_option(
        int         level,
        int         optname,
        void const* data,
        std::size_t size) noexcept override
    {
        if (::setsockopt(
                fd_, level, optname,
                reinterpret_cast<char const*>(data),
                static_cast<socklen_t>(size)) != 0)
            return make_err(errno);
        return {};
    }

    std::error_code get_option(
        int         level,
        int         optname,
        void*       data,
        std::size_t* size) const noexcept override
    {
        socklen_t len = static_cast<socklen_t>(*size);
        if (::getsockopt(fd_, level, optname,
                reinterpret_cast<char*>(data), &len) != 0)
            return make_err(errno);
        *size = static_cast<std::size_t>(len);
        return {};
    }

    corosio::local_endpoint local_endpoint() const noexcept override
    {
        return local_endpoint_;
    }

    corosio::local_endpoint remote_endpoint() const noexcept override
    {
        return remote_endpoint_;
    }
};

/** Unix domain stream socket service for io_uring.

    Owns all `io_uring_local_stream_socket` implementations for an
    `io_context`. Satisfies the `local_stream_service` interface so the
    generic `local_stream_socket` front-end can call `open_socket` and
    `assign_socket` transparently.

    Socket impls are reference-counted inside the service map; raw
    pointers returned from `construct()` remain valid until `destroy()`
    or `shutdown()` is called.

    @par Thread Safety
    All public member functions are thread-safe.
*/
class BOOST_COROSIO_DECL io_uring_local_stream_service final
    : public local_stream_service
{
public:
    /// Identifies this service for `execution_context` lookup.
    using key_type = local_stream_service;

    /** Construct the local stream service.

        @param ctx The owning execution context. The io_uring scheduler
            must already be registered.
    */
    explicit io_uring_local_stream_service(capy::execution_context& ctx)
        : sched_(&ctx.use_service<io_uring_scheduler>())
    {}

    void shutdown() override
    {
        std::vector<std::shared_ptr<io_uring_local_stream_socket>> live;
        {
            std::lock_guard lk(mutex_);
            live.reserve(impls_.size());
            for (auto& [_, p] : impls_)
                live.push_back(p);
        }
        // Cancel without the lock held to avoid inversion if cancel()
        // ever needs to re-enter the service.
        for (auto& p : live)
            p->cancel();
    }

    io_object::implementation* construct() override
    {
        auto p   = std::make_shared<io_uring_local_stream_socket>(*this, *sched_);
        auto* raw = p.get();
        std::lock_guard lk(mutex_);
        impls_.emplace(raw, std::move(p));
        return raw;
    }

    void destroy(io_object::implementation* p) override
    {
        if (!p)
            return;
        std::lock_guard lk(mutex_);
        impls_.erase(static_cast<io_uring_local_stream_socket*>(p));
    }

    // Close the fd eagerly when local_stream_socket::close() is called,
    // before destroy() drops the shared_ptr and the destructor runs.
    void close(io_object::handle& h) override
    {
        auto* sock = static_cast<io_uring_local_stream_socket*>(h.get());
        if (sock && sock->fd_ >= 0)
        {
            // Cancel pending SQEs before closing. The cancel SQE must
            // be submitted to the kernel while the fd is still open;
            // otherwise IORING_ASYNC_CANCEL_FD resolves to the wrong
            // file if the fd number is immediately recycled.
            sched_->cancel_and_flush(sock->fd_);
            ::close(sock->fd_);
            sock->fd_              = -1;
            sock->local_endpoint_  = corosio::local_endpoint{};
            sock->remote_endpoint_ = corosio::local_endpoint{};
        }
    }

    /** Open an AF_UNIX stream socket and associate it with an impl.

        Creates a non-blocking, close-on-exec socket via `socket(2)`.
        `family` is always `AF_UNIX` for local stream sockets.

        @param impl     The socket implementation to initialise.
        @param family   Address family (`AF_UNIX`).
        @param type     Socket type (`SOCK_STREAM`).
        @param protocol Protocol number (typically 0).
        @return Error code on failure, empty on success.
    */
    std::error_code open_socket(
        local_stream_socket::implementation& impl,
        int family, int type, int protocol) override
    {
        auto& sock = static_cast<io_uring_local_stream_socket&>(impl);
        int fd = ::socket(family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
        if (fd < 0)
            return make_err(errno);
        if (sock.fd_ >= 0)
        {
            sched_->submit_cancel_by_fd(sock.fd_);
            ::close(sock.fd_);
        }
        sock.fd_ = fd;
        return {};
    }

    /** Adopt a pre-created fd into an impl (e.g. from `socketpair`).

        Takes ownership of `fd` on success; the caller retains ownership
        on failure.

        @param impl The socket implementation to assign to.
        @param fd   A valid, open, non-blocking AF_UNIX stream fd.
        @return Error code on failure, empty on success.
    */
    std::error_code assign_socket(
        local_stream_socket::implementation& impl,
        native_handle_type fd) override
    {
        auto& sock = static_cast<io_uring_local_stream_socket&>(impl);
        if (sock.fd_ >= 0)
        {
            sched_->cancel_and_flush(sock.fd_);
            ::close(sock.fd_);
        }
        sock.fd_ = static_cast<int>(fd);

        sockaddr_storage local{};
        socklen_t local_len = sizeof(local);
        if (::getsockname(sock.fd_,
                reinterpret_cast<sockaddr*>(&local), &local_len) == 0)
            sock.local_endpoint_ = sockaddr_to_local_endpoint(local, local_len);

        sockaddr_storage remote{};
        socklen_t remote_len = sizeof(remote);
        if (::getpeername(sock.fd_,
                reinterpret_cast<sockaddr*>(&remote), &remote_len) == 0)
            sock.remote_endpoint_ = sockaddr_to_local_endpoint(remote, remote_len);

        return {};
    }

    /** Wrap an already-accepted fd as a new socket impl.

        Called by the acceptor service after `accept(2)` returns a
        connected fd. Captures both endpoints via the provided peer
        address and a `getsockname` call.

        @param fd   Accepted file descriptor (must be non-blocking).
        @param peer Peer endpoint from `accept(2)`.
        @return Raw pointer to the registered impl.
    */
    io_uring_local_stream_socket* adopt_fd(
        int fd, corosio::local_endpoint const& peer)
    {
        auto p = std::make_shared<io_uring_local_stream_socket>(*this, *sched_);
        p->fd_              = fd;
        p->remote_endpoint_ = peer;

        sockaddr_storage local{};
        socklen_t len = sizeof(local);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) == 0)
            p->local_endpoint_ = sockaddr_to_local_endpoint(local, len);

        std::lock_guard lk(mutex_);
        auto* raw = p.get();
        impls_.emplace(raw, std::move(p));
        return raw;
    }

    /// Return the scheduler used by sockets created by this service.
    io_uring_scheduler& scheduler() noexcept { return *sched_; }

private:
    io_uring_scheduler*  sched_;
    std::mutex           mutex_;
    std::unordered_map<io_uring_local_stream_socket*,
                       std::shared_ptr<io_uring_local_stream_socket>> impls_;
};

/** Local-stream (Unix domain) acceptor for io_uring.

    Inherits all multishot machinery (parked-fd queue, waiter queue,
    CQE drain on destruction) from `io_uring_multishot_acceptor_base`.
    Adds only the `accept()` override, the `adopt_thunk` static that
    wraps an accepted fd via `io_uring_local_stream_service::adopt_fd`,
    and `release_socket()` (a pure virtual in
    `local_stream_acceptor::implementation` absent from the base).
*/
class BOOST_COROSIO_DECL io_uring_local_stream_acceptor final
    : public io_uring_multishot_acceptor_base<
          io_uring_local_stream_acceptor,
          local_stream_acceptor::implementation,
          corosio::local_endpoint,
          io_uring_local_stream_service>
{
    friend io_uring_local_stream_acceptor_service;

    using base_type = io_uring_multishot_acceptor_base<
        io_uring_local_stream_acceptor,
        local_stream_acceptor::implementation,
        corosio::local_endpoint,
        io_uring_local_stream_service>;

public:
    explicit io_uring_local_stream_acceptor(
        io_uring_local_stream_acceptor_service&,
        io_uring_scheduler&            sched,
        io_uring_local_stream_service& peer_svc) noexcept
        : base_type(sched, peer_svc)
    {}

    std::coroutine_handle<> accept(
        std::coroutine_handle<>     h,
        capy::executor_ref          ex,
        std::stop_token             token,
        std::error_code*            ec,
        io_object::implementation** impl_out) override
    {
        base_type::dispatch_or_queue(h, ex, std::move(token), ec, impl_out);
        return std::noop_coroutine();
    }

    // release_socket() is pure virtual in local_stream_acceptor::implementation
    // but not in tcp_acceptor::implementation, so the base does not cover it.
    native_handle_type release_socket() noexcept override
    {
        int fd = fd_;
        fd_ = -1;
        local_endpoint_ = corosio::local_endpoint{};
        return fd;
    }

    static io_object::implementation* adopt_thunk(
        void* peer_service, int fd,
        sockaddr_storage const& peer, socklen_t peer_len) noexcept
    {
        auto* svc = static_cast<io_uring_local_stream_service*>(peer_service);
        return svc->adopt_fd(fd, sockaddr_to_local_endpoint(peer, peer_len));
    }
};

/** Unix domain stream acceptor service for io_uring.

    Owns all `io_uring_local_stream_acceptor` implementations for an
    `io_context`. Satisfies the `local_stream_acceptor_service` interface
    so the generic `local_stream_acceptor` front-end can call
    `open_acceptor_socket`, `bind_acceptor`, and `listen_acceptor`
    transparently.

    Acceptor impls are reference-counted inside the service map; raw
    pointers returned from `construct()` remain valid until `destroy()`
    or `shutdown()` is called.

    @par Thread Safety
    All public member functions are thread-safe.
*/
class BOOST_COROSIO_DECL io_uring_local_stream_acceptor_service final
    : public local_stream_acceptor_service
{
public:
    /// Identifies this service for `execution_context` lookup.
    using key_type = local_stream_acceptor_service;

    /** Construct the local stream acceptor service.

        @param ctx The owning execution context. Both the io_uring scheduler
            and the local stream socket service must already be registered.
    */
    explicit io_uring_local_stream_acceptor_service(capy::execution_context& ctx)
        : sched_(&ctx.use_service<io_uring_scheduler>())
        , peer_svc_(&ctx.use_service<io_uring_local_stream_service>())
    {}

    void shutdown() override
    {
        std::vector<std::shared_ptr<io_uring_local_stream_acceptor>> live;
        {
            std::lock_guard lk(mutex_);
            live.reserve(impls_.size());
            for (auto& [_, p] : impls_)
                live.push_back(p);
        }
        // Cancel without the lock held to avoid inversion if cancel()
        // re-enters the service.
        for (auto& p : live)
            p->cancel();
    }

    io_object::implementation* construct() override
    {
        auto p   = std::make_shared<io_uring_local_stream_acceptor>(
            *this, *sched_, *peer_svc_);
        auto* raw = p.get();
        std::lock_guard lk(mutex_);
        impls_.emplace(raw, std::move(p));
        return raw;
    }

    void destroy(io_object::implementation* p) override
    {
        if (!p)
            return;
        std::lock_guard lk(mutex_);
        impls_.erase(static_cast<io_uring_local_stream_acceptor*>(p));
    }

    // Close the fd eagerly when local_stream_acceptor::close() is called,
    // before destroy() drops the shared_ptr and the destructor runs.
    void close(io_object::handle& h) override
    {
        auto* acc = static_cast<io_uring_local_stream_acceptor*>(h.get());
        if (acc && acc->fd_ >= 0)
        {
            // cancel_and_flush submits cancel-by-fd; drain_waiters_only
            // drains queued waiters without re-submitting it.
            sched_->cancel_and_flush(acc->fd_);
            acc->drain_waiters_only();
            ::close(acc->fd_);
            acc->fd_ = -1;

            // Break the multi_op_ -> impl_ptr (shared_ptr<this>) cycle
            // start_multishot established. See the symmetric comment
            // in io_uring_tcp_acceptor_service::close.
            if (acc->multi_op_)
                acc->multi_op_->impl_ptr.reset();
        }
    }

    /** Create a non-blocking, close-on-exec AF_UNIX socket for accepting.

        @param impl     The acceptor implementation to initialise.
        @param family   Address family (`AF_UNIX`).
        @param type     Socket type (`SOCK_STREAM`).
        @param protocol Protocol number (typically 0).
        @return Error code on failure, empty on success.
    */
    std::error_code open_acceptor_socket(
        local_stream_acceptor::implementation& impl,
        int family,
        int type,
        int protocol) override
    {
        auto& acc = static_cast<io_uring_local_stream_acceptor&>(impl);
        int fd = ::socket(family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
        if (fd < 0)
            return make_err(errno);
        if (acc.fd_ >= 0)
        {
            sched_->submit_cancel_by_fd(acc.fd_);
            ::close(acc.fd_);
        }
        acc.fd_ = fd;
        return {};
    }

    /** Bind an open acceptor and capture the local endpoint.

        @param impl The acceptor implementation to bind.
        @param ep   The local endpoint (path) to bind to.
        @return Error code on failure, empty on success.
    */
    std::error_code bind_acceptor(
        local_stream_acceptor::implementation& impl,
        corosio::local_endpoint ep) override
    {
        auto& acc = static_cast<io_uring_local_stream_acceptor&>(impl);
        sockaddr_storage addr{};
        socklen_t len = endpoint_to_sockaddr(ep, addr);
        if (::bind(acc.fd_, reinterpret_cast<sockaddr*>(&addr), len) < 0)
            return make_err(errno);

        sockaddr_storage local{};
        socklen_t local_len = sizeof(local);
        if (::getsockname(
                acc.fd_,
                reinterpret_cast<sockaddr*>(&local), &local_len) == 0)
            acc.local_endpoint_ = sockaddr_to_local_endpoint(local, local_len);
        return {};
    }

    /** Start listening and submit the multishot accept SQE.

        Calls `::listen(2)` then arms the io_uring multishot accept
        operation that delivers one CQE per accepted connection.

        @param impl    The acceptor implementation to listen on.
        @param backlog Maximum pending-connection queue length.
        @return Error code on failure, empty on success.
    */
    std::error_code listen_acceptor(
        local_stream_acceptor::implementation& impl,
        int backlog) override
    {
        auto& acc = static_cast<io_uring_local_stream_acceptor&>(impl);
        if (::listen(acc.fd_, backlog) < 0)
            return make_err(errno);
        acc.start_multishot();
        return {};
    }

    /// Return the scheduler used by acceptors created by this service.
    io_uring_scheduler& scheduler() noexcept { return *sched_; }

private:
    io_uring_scheduler*             sched_;
    io_uring_local_stream_service*  peer_svc_;
    std::mutex                      mutex_;
    std::unordered_map<io_uring_local_stream_acceptor*,
        std::shared_ptr<io_uring_local_stream_acceptor>> impls_;
};

/** UDP socket implementation for io_uring.

    Implements `udp_socket::implementation` using a proactor model:
    send_to, recv_from, send, recv, and connect operations are submitted
    to the kernel via `io_uring_submit_op` and complete through the ring's
    CQE path.

    The object is always owned by a `shared_ptr` managed by the service.
    In-flight ops hold an additional `shared_ptr` copy (`impl_ptr`) so
    the kernel's user-data pointer remains valid until the CQE arrives.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Unsafe. One send and one recv may be in flight
    simultaneously, but two sends or two recvs must not overlap.
*/
class BOOST_COROSIO_DECL io_uring_udp_socket final
    : public udp_socket::implementation
    , public std::enable_shared_from_this<io_uring_udp_socket>
{
    friend io_uring_udp_service;

    int                    fd_     = -1;
    int                    family_ = AF_UNSPEC;  // cached at open_socket
    io_uring_scheduler*    sched_  = nullptr;
    io_uring_udp_service*  svc_    = nullptr;

    corosio::endpoint local_endpoint_;
    corosio::endpoint remote_endpoint_;

    // Per-fd op slots — embedded to eliminate per-call heap allocation.
    // Single-pending invariant per slot.
    uring_connect_op    conn_;
    uring_dgram_send_op send_;
    uring_dgram_recv_op recv_;

    mutable detail::speculative_state spec_;

public:
    /** Construct with service and scheduler references.

        Both refs must outlive this socket.

        @param svc   The owning service.
        @param sched The io_uring scheduler owned by the context.
    */
    explicit io_uring_udp_socket(
        io_uring_udp_service& svc,
        io_uring_scheduler&   sched) noexcept
        : sched_(&sched)
        , svc_(&svc)
    {}

    ~io_uring_udp_socket() override
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

    // ----------------------------------------------------------------
    // udp_socket::implementation
    // ----------------------------------------------------------------

    std::coroutine_handle<> send_to(
        std::coroutine_handle<> h,
        capy::executor_ref      ex,
        buffer_param            buf,
        endpoint                dest,
        int                     flags,
        std::stop_token         token,
        std::error_code*        ec,
        std::size_t*            bytes_out) override
    {
        sockaddr_storage addr{};
        socklen_t len = endpoint_to_sockaddr(dest, addr);
        return submit_send(h, ex, buf, len, addr, flags,
            std::move(token), ec, bytes_out);
    }

    std::coroutine_handle<> recv_from(
        std::coroutine_handle<> h,
        capy::executor_ref      ex,
        buffer_param            buf,
        endpoint*               source,
        int                     flags,
        std::stop_token         token,
        std::error_code*        ec,
        std::size_t*            bytes_out) override
    {
        return submit_recv(h, ex, buf, source != nullptr, source, flags,
            std::move(token), ec, bytes_out);
    }

    std::coroutine_handle<> send(
        std::coroutine_handle<> h,
        capy::executor_ref      ex,
        buffer_param            buf,
        int                     flags,
        std::stop_token         token,
        std::error_code*        ec,
        std::size_t*            bytes_out) override
    {
        sockaddr_storage empty{};
        return submit_send(h, ex, buf, 0, empty, flags,
            std::move(token), ec, bytes_out);
    }

    std::coroutine_handle<> recv(
        std::coroutine_handle<> h,
        capy::executor_ref      ex,
        buffer_param            buf,
        int                     flags,
        std::stop_token         token,
        std::error_code*        ec,
        std::size_t*            bytes_out) override
    {
        return submit_recv(h, ex, buf, false, nullptr, flags,
            std::move(token), ec, bytes_out);
    }

    std::coroutine_handle<> connect(
        std::coroutine_handle<> h,
        capy::executor_ref      ex,
        endpoint                ep,
        std::stop_token         token,
        std::error_code*        ec) override
    {
        bool stop_now = token.stop_possible() && token.stop_requested();
        if (stop_now)
        {
            if (sched_->try_consume_inline_budget())
            {
                if (ec) *ec = capy::error::canceled;
                conn_.cont_op.cont.h = h;
                return dispatch_coro(ex, conn_.cont_op.cont);
            }
            conn_.addrlen = to_sockaddr(ep, family_, conn_.addr);
            conn_.prepare(h, ex, ec, fd_, sched_, shared_from_this(),
                ep, &remote_endpoint_, &local_endpoint_, token);
            conn_.cancelled.store(true, std::memory_order_release);
            sched_->work_started();
            {
                io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
                sched_->push_completed_locked(&conn_);
            }
            return std::noop_coroutine();
        }

        // io_uring's IORING_OP_CONNECT re-invokes connect(2) internally;
        // a prior speculative ::connect would leave EINPROGRESS → EALREADY.
        conn_.addrlen = to_sockaddr(ep, family_, conn_.addr);
        conn_.prepare(h, ex, ec, fd_, sched_, shared_from_this(),
            ep, &remote_endpoint_, &local_endpoint_, token);
        sched_->work_started();
        if (conn_.cancelled.load(std::memory_order_acquire))
        {
            io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
            sched_->push_completed_locked(&conn_);
            return std::noop_coroutine();
        }
        io_uring_submit_op(*sched_, &conn_);
        return std::noop_coroutine();
    }

    native_handle_type native_handle() const noexcept override
    {
        return fd_;
    }

    void cancel() noexcept override
    {
        if (fd_ >= 0)
            sched_->submit_cancel_by_fd(fd_);
    }

    std::error_code set_option(
        int         level,
        int         optname,
        void const* data,
        std::size_t size) noexcept override
    {
        if (::setsockopt(
                fd_, level, optname,
                reinterpret_cast<char const*>(data),
                static_cast<socklen_t>(size)) != 0)
            return make_err(errno);
        return {};
    }

    std::error_code get_option(
        int          level,
        int          optname,
        void*        data,
        std::size_t* size) const noexcept override
    {
        socklen_t len = static_cast<socklen_t>(*size);
        if (::getsockopt(fd_, level, optname,
                reinterpret_cast<char*>(data), &len) != 0)
            return make_err(errno);
        *size = static_cast<std::size_t>(len);
        return {};
    }

    endpoint local_endpoint() const noexcept override
    {
        return local_endpoint_;
    }

    endpoint remote_endpoint() const noexcept override
    {
        return remote_endpoint_;
    }

private:
    std::coroutine_handle<> submit_send(
        std::coroutine_handle<>        h,
        capy::executor_ref             ex,
        buffer_param                   buffers,
        socklen_t                      dest_len,
        sockaddr_storage const&        dest_storage,
        int                            flags,
        std::stop_token                token,
        std::error_code*               ec,
        std::size_t*                   bytes)
    {
        iovec iovecs[io_uring_max_iov];
        int   iovec_count = static_cast<int>(
            buffers.copy_to(
                reinterpret_cast<capy::mutable_buffer*>(iovecs),
                io_uring_max_iov));
        bool stop_now  = token.stop_possible() && token.stop_requested();
        bool empty_buf = (iovec_count == 0);

        ssize_t n             = 0;
        int     err           = 0;
        bool    have_sync_res = stop_now || empty_buf;
        if (!have_sync_res && spec_.may_speculate_write())
        {
            msghdr msg{};
            msg.msg_iov    = iovecs;
            msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(iovec_count);
            sockaddr_storage dest_copy = dest_storage;
            if (dest_len > 0)
            {
                msg.msg_name    = &dest_copy;
                msg.msg_namelen = dest_len;
            }
            int native_flags = to_native_msg_flags(flags) | MSG_NOSIGNAL;
            do { n = ::sendmsg(fd_, &msg, native_flags); }
            while (n < 0 && errno == EINTR);
            if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            {
                have_sync_res = true;
                if (n < 0) err = errno;
            }
            else
            {
                spec_.on_write_exhausted();
            }
        }

        if (have_sync_res)
        {
            if (sched_->try_consume_inline_budget())
            {
                if (ec)
                    *ec = stop_now ? capy::error::canceled
                          : err   ? make_err(err)
                                  : std::error_code{};
                if (bytes)
                    *bytes = (n < 0) ? 0u : static_cast<std::size_t>(n);
                send_.cont_op.cont.h = h;
                return dispatch_coro(ex, send_.cont_op.cont);
            }
            send_.prepare(h, ex, ec, bytes, fd_, sched_,
                shared_from_this(), &spec_, buffers, dest_len, dest_storage,
                to_native_msg_flags(flags), token);
            if (stop_now)
                send_.cancelled.store(true, std::memory_order_release);
            else
                send_.res = (n < 0) ? -err : static_cast<int>(n);
            sched_->work_started();
            {
                io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
                sched_->push_completed_locked(&send_);
            }
            return std::noop_coroutine();
        }

        send_.prepare(h, ex, ec, bytes, fd_, sched_, shared_from_this(),
            &spec_, buffers, dest_len, dest_storage,
            to_native_msg_flags(flags), token);
        sched_->work_started();
        if (send_.cancelled.load(std::memory_order_acquire))
        {
            io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
            sched_->push_completed_locked(&send_);
            return std::noop_coroutine();
        }
        io_uring_submit_op(*sched_, &send_);
        return std::noop_coroutine();
    }

    std::coroutine_handle<> submit_recv(
        std::coroutine_handle<>  h,
        capy::executor_ref       ex,
        buffer_param             buffers,
        bool                     want_source,
        corosio::endpoint*       source_out,
        int                      flags,
        std::stop_token          token,
        std::error_code*         ec,
        std::size_t*             bytes)
    {
        iovec iovecs[io_uring_max_iov];
        int   iovec_count = static_cast<int>(
            buffers.copy_to(
                reinterpret_cast<capy::mutable_buffer*>(iovecs),
                io_uring_max_iov));
        bool stop_now  = token.stop_possible() && token.stop_requested();
        bool empty_buf = (iovec_count == 0);

        ssize_t          n             = 0;
        int              err           = 0;
        bool             have_sync_res = stop_now || empty_buf;
        sockaddr_storage src_storage{};
        socklen_t        src_namelen   = 0;
        if (!have_sync_res && spec_.may_speculate_read())
        {
            msghdr msg{};
            msg.msg_iov    = iovecs;
            msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(iovec_count);
            if (want_source)
            {
                msg.msg_name    = &src_storage;
                msg.msg_namelen = sizeof(src_storage);
            }
            int native_flags = to_native_msg_flags(flags);
            do { n = ::recvmsg(fd_, &msg, native_flags); }
            while (n < 0 && errno == EINTR);
            if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            {
                have_sync_res = true;
                if (n < 0) err = errno;
                src_namelen = (n >= 0) ? msg.msg_namelen : 0;
            }
            else
            {
                spec_.on_read_exhausted();
            }
        }

        if (have_sync_res)
        {
            if (sched_->try_consume_inline_budget())
            {
                if (ec)
                    *ec = stop_now ? capy::error::canceled
                          : err   ? make_err(err)
                                  : std::error_code{};
                if (bytes)
                    *bytes = (n < 0) ? 0u : static_cast<std::size_t>(n);
                if (n >= 0 && want_source && source_out && !empty_buf)
                    *source_out = sockaddr_to_endpoint(src_storage);
                recv_.cont_op.cont.h = h;
                return dispatch_coro(ex, recv_.cont_op.cont);
            }
            recv_.prepare(h, ex, ec, bytes, fd_, sched_, shared_from_this(),
                &spec_, buffers, source_out,
                want_source ? &write_ip_source : nullptr,
                to_native_msg_flags(flags), token);
            if (stop_now)
                recv_.cancelled.store(true, std::memory_order_release);
            else
            {
                recv_.res = (n < 0) ? -err : static_cast<int>(n);
                // Hand the speculative source over to do_handler's
                // source_writer so it translates into source_out the same
                // way the kernel-completed path would.
                if (n >= 0 && want_source)
                {
                    recv_.source_storage = src_storage;
                    recv_.source_len     = src_namelen;
                }
            }
            sched_->work_started();
            {
                io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
                sched_->push_completed_locked(&recv_);
            }
            return std::noop_coroutine();
        }

        recv_.prepare(h, ex, ec, bytes, fd_, sched_, shared_from_this(),
            &spec_, buffers, source_out,
            want_source ? &write_ip_source : nullptr,
            to_native_msg_flags(flags), token);
        sched_->work_started();
        if (recv_.iovec_count == 0 ||
            recv_.cancelled.load(std::memory_order_acquire))
        {
            io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
            sched_->push_completed_locked(&recv_);
            return std::noop_coroutine();
        }
        io_uring_submit_op(*sched_, &recv_);
        return std::noop_coroutine();
    }

    static void write_ip_source(
        void* ctx, sockaddr_storage const& s, socklen_t /*len*/) noexcept
    {
        if (auto* out = static_cast<corosio::endpoint*>(ctx))
            *out = sockaddr_to_endpoint(s);
    }
};

/** UDP socket service for io_uring.

    Owns all `io_uring_udp_socket` implementations for an `io_context`.
    Satisfies the `udp_service` interface so the generic `udp_socket`
    front-end can call `open_datagram_socket` and `bind_datagram`
    transparently.

    Socket impls are reference-counted inside the service map; raw
    pointers returned from `construct()` remain valid until `destroy()`
    or `shutdown()` is called.

    @par Thread Safety
    All public member functions are thread-safe.
*/
class BOOST_COROSIO_DECL io_uring_udp_service final
    : public udp_service
{
public:
    /// Identifies this service for `execution_context` lookup.
    using key_type = udp_service;

    /** Construct the UDP service.

        @param ctx The owning execution context. The io_uring scheduler
            must already be registered.
    */
    explicit io_uring_udp_service(capy::execution_context& ctx)
        : sched_(&ctx.use_service<io_uring_scheduler>())
    {}

    void shutdown() override
    {
        std::vector<std::shared_ptr<io_uring_udp_socket>> live;
        {
            std::lock_guard lk(mutex_);
            live.reserve(impls_.size());
            for (auto& [_, p] : impls_)
                live.push_back(p);
        }
        // Cancel without the lock held to avoid inversion if cancel()
        // ever needs to re-enter the service.
        for (auto& p : live)
            p->cancel();
    }

    io_object::implementation* construct() override
    {
        auto p   = std::make_shared<io_uring_udp_socket>(*this, *sched_);
        auto* raw = p.get();
        std::lock_guard lk(mutex_);
        impls_.emplace(raw, std::move(p));
        return raw;
    }

    void destroy(io_object::implementation* p) override
    {
        if (!p)
            return;
        std::lock_guard lk(mutex_);
        impls_.erase(static_cast<io_uring_udp_socket*>(p));
    }

    // Close the fd eagerly when udp_socket::close() is called, before
    // destroy() drops the shared_ptr and the destructor runs.
    void close(io_object::handle& h) override
    {
        auto* sock = static_cast<io_uring_udp_socket*>(h.get());
        if (sock && sock->fd_ >= 0)
        {
            // Cancel pending SQEs before closing so the kernel resolves
            // the fd number while it is still valid.
            sched_->cancel_and_flush(sock->fd_);
            ::close(sock->fd_);
            sock->fd_              = -1;
            sock->local_endpoint_  = endpoint{};
            sock->remote_endpoint_ = endpoint{};
        }
    }

    /** Open a datagram socket and associate it with an impl.

        Creates a non-blocking, close-on-exec socket via `socket(2)`.

        @param impl     The socket implementation to initialise.
        @param family   Address family (e.g. `AF_INET`, `AF_INET6`).
        @param type     Socket type (`SOCK_DGRAM`).
        @param protocol Protocol number (`IPPROTO_UDP`).
        @return Error code on failure, empty on success.
    */
    std::error_code open_datagram_socket(
        udp_socket::implementation& impl,
        int family, int type, int protocol) override
    {
        auto& sock = static_cast<io_uring_udp_socket&>(impl);
        int fd = ::socket(
            family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
        if (fd < 0)
            return make_err(errno);
        if (sock.fd_ >= 0)
        {
            sched_->submit_cancel_by_fd(sock.fd_);
            ::close(sock.fd_);
        }
        sock.fd_     = fd;
        sock.family_ = family;
        if (family == AF_INET6)
        {
            int one = 1;
            ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
        }
        return {};
    }

    /** Bind the socket and capture the local endpoint via `getsockname`.

        @param impl The socket implementation to bind.
        @param ep   The local endpoint to bind to.
        @return Error code on failure, empty on success.
    */
    std::error_code bind_datagram(
        udp_socket::implementation& impl, endpoint ep) override
    {
        auto& sock = static_cast<io_uring_udp_socket&>(impl);
        sockaddr_storage addr{};
        socklen_t len = endpoint_to_sockaddr(ep, addr);
        if (::bind(
                sock.fd_,
                reinterpret_cast<sockaddr*>(&addr), len) < 0)
            return make_err(errno);

        sockaddr_storage local{};
        socklen_t local_len = sizeof(local);
        if (::getsockname(
                sock.fd_,
                reinterpret_cast<sockaddr*>(&local), &local_len) == 0)
            sock.local_endpoint_ = sockaddr_to_endpoint(local);
        return {};
    }

    /// Return the scheduler used by sockets created by this service.
    io_uring_scheduler& scheduler() noexcept { return *sched_; }

private:
    io_uring_scheduler*  sched_;
    std::mutex           mutex_;
    std::unordered_map<io_uring_udp_socket*,
                       std::shared_ptr<io_uring_udp_socket>> impls_;
};

/** Unix domain datagram socket implementation for io_uring.

    Implements `local_datagram_socket::implementation` using a proactor
    model: send_to, recv_from, send, recv, and connect operations are
    submitted to the kernel via `io_uring_submit_op` and complete through
    the ring's CQE path.

    The object is always owned by a `shared_ptr` managed by the service.
    In-flight ops hold an additional `shared_ptr` copy (`impl_ptr`) so
    the kernel's user-data pointer remains valid until the CQE arrives.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Unsafe. One send and one recv may be in flight
    simultaneously, but two sends or two recvs must not overlap.
*/
class BOOST_COROSIO_DECL io_uring_local_datagram_socket final
    : public local_datagram_socket::implementation
    , public std::enable_shared_from_this<io_uring_local_datagram_socket>
{
    friend io_uring_local_datagram_service;

    int                              fd_    = -1;
    io_uring_scheduler*              sched_ = nullptr;
    io_uring_local_datagram_service* svc_   = nullptr;

    corosio::local_endpoint local_endpoint_;
    corosio::local_endpoint remote_endpoint_;

    // Per-fd op slots — embedded to eliminate per-call heap allocation.
    // Single-pending invariant per slot.
    uring_local_connect_op conn_;
    uring_dgram_send_op    send_;
    uring_dgram_recv_op    recv_;

    mutable detail::speculative_state spec_;

public:
    /** Construct with service and scheduler references.

        Both refs must outlive this socket.

        @param svc   The owning service.
        @param sched The io_uring scheduler owned by the context.
    */
    explicit io_uring_local_datagram_socket(
        io_uring_local_datagram_service& svc,
        io_uring_scheduler&              sched) noexcept
        : sched_(&sched)
        , svc_(&svc)
    {}

    ~io_uring_local_datagram_socket() override
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

    // ----------------------------------------------------------------
    // local_datagram_socket::implementation
    // ----------------------------------------------------------------

    std::coroutine_handle<> send_to(
        std::coroutine_handle<>  h,
        capy::executor_ref       ex,
        buffer_param             buf,
        corosio::local_endpoint  dest,
        int                      flags,
        std::stop_token          token,
        std::error_code*         ec,
        std::size_t*             bytes_out) override
    {
        sockaddr_storage addr{};
        socklen_t len = endpoint_to_sockaddr(dest, addr);
        return submit_send(h, ex, buf, len, addr, flags,
            std::move(token), ec, bytes_out);
    }

    std::coroutine_handle<> recv_from(
        std::coroutine_handle<>    h,
        capy::executor_ref         ex,
        buffer_param               buf,
        corosio::local_endpoint*   source,
        int                        flags,
        std::stop_token            token,
        std::error_code*           ec,
        std::size_t*               bytes_out) override
    {
        return submit_recv(h, ex, buf, source != nullptr, source, flags,
            std::move(token), ec, bytes_out);
    }

    std::coroutine_handle<> send(
        std::coroutine_handle<> h,
        capy::executor_ref      ex,
        buffer_param            buf,
        int                     flags,
        std::stop_token         token,
        std::error_code*        ec,
        std::size_t*            bytes_out) override
    {
        sockaddr_storage empty{};
        return submit_send(h, ex, buf, 0, empty, flags,
            std::move(token), ec, bytes_out);
    }

    std::coroutine_handle<> recv(
        std::coroutine_handle<> h,
        capy::executor_ref      ex,
        buffer_param            buf,
        int                     flags,
        std::stop_token         token,
        std::error_code*        ec,
        std::size_t*            bytes_out) override
    {
        return submit_recv(h, ex, buf, false, nullptr, flags,
            std::move(token), ec, bytes_out);
    }

    std::coroutine_handle<> connect(
        std::coroutine_handle<>  h,
        capy::executor_ref       ex,
        corosio::local_endpoint  ep,
        std::stop_token          token,
        std::error_code*         ec) override
    {
        bool stop_now = token.stop_possible() && token.stop_requested();
        if (stop_now)
        {
            if (sched_->try_consume_inline_budget())
            {
                if (ec) *ec = capy::error::canceled;
                conn_.cont_op.cont.h = h;
                return dispatch_coro(ex, conn_.cont_op.cont);
            }
            conn_.addrlen = to_sockaddr(ep, conn_.addr);
            conn_.prepare(h, ex, ec, fd_, sched_, shared_from_this(),
                ep, &remote_endpoint_, &local_endpoint_, token);
            conn_.cancelled.store(true, std::memory_order_release);
            sched_->work_started();
            {
                io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
                sched_->push_completed_locked(&conn_);
            }
            return std::noop_coroutine();
        }

        // io_uring's IORING_OP_CONNECT re-invokes connect(2) internally;
        // a prior speculative ::connect would leave EINPROGRESS → EALREADY.
        conn_.addrlen = to_sockaddr(ep, conn_.addr);
        conn_.prepare(h, ex, ec, fd_, sched_, shared_from_this(),
            ep, &remote_endpoint_, &local_endpoint_, token);
        sched_->work_started();
        if (conn_.cancelled.load(std::memory_order_acquire))
        {
            io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
            sched_->push_completed_locked(&conn_);
            return std::noop_coroutine();
        }
        io_uring_submit_op(*sched_, &conn_);
        return std::noop_coroutine();
    }

    std::error_code shutdown(
        local_datagram_socket::shutdown_type what) noexcept override
    {
        if (::shutdown(fd_, static_cast<int>(what)) != 0)
            return make_err(errno);
        return {};
    }

    native_handle_type native_handle() const noexcept override
    {
        return fd_;
    }

    native_handle_type release_socket() noexcept override
    {
        int fd = fd_;
        fd_ = -1;
        local_endpoint_  = corosio::local_endpoint{};
        remote_endpoint_ = corosio::local_endpoint{};
        return fd;
    }

    void cancel() noexcept override
    {
        if (fd_ >= 0)
            sched_->submit_cancel_by_fd(fd_);
    }

    std::error_code set_option(
        int         level,
        int         optname,
        void const* data,
        std::size_t size) noexcept override
    {
        if (::setsockopt(
                fd_, level, optname,
                reinterpret_cast<char const*>(data),
                static_cast<socklen_t>(size)) != 0)
            return make_err(errno);
        return {};
    }

    std::error_code get_option(
        int          level,
        int          optname,
        void*        data,
        std::size_t* size) const noexcept override
    {
        socklen_t len = static_cast<socklen_t>(*size);
        if (::getsockopt(fd_, level, optname,
                reinterpret_cast<char*>(data), &len) != 0)
            return make_err(errno);
        *size = static_cast<std::size_t>(len);
        return {};
    }

    corosio::local_endpoint local_endpoint() const noexcept override
    {
        return local_endpoint_;
    }

    corosio::local_endpoint remote_endpoint() const noexcept override
    {
        return remote_endpoint_;
    }

    std::error_code bind(corosio::local_endpoint ep) noexcept override
    {
        sockaddr_storage addr{};
        socklen_t len = endpoint_to_sockaddr(ep, addr);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), len) != 0)
            return make_err(errno);

        sockaddr_storage local{};
        socklen_t local_len = sizeof(local);
        if (::getsockname(
                fd_,
                reinterpret_cast<sockaddr*>(&local), &local_len) == 0)
            local_endpoint_ = sockaddr_to_local_endpoint(local, local_len);
        return {};
    }

private:
    std::coroutine_handle<> submit_send(
        std::coroutine_handle<>        h,
        capy::executor_ref             ex,
        buffer_param                   buffers,
        socklen_t                      dest_len,
        sockaddr_storage const&        dest_storage,
        int                            flags,
        std::stop_token                token,
        std::error_code*               ec,
        std::size_t*                   bytes)
    {
        iovec iovecs[io_uring_max_iov];
        int   iovec_count = static_cast<int>(
            buffers.copy_to(
                reinterpret_cast<capy::mutable_buffer*>(iovecs),
                io_uring_max_iov));
        bool stop_now  = token.stop_possible() && token.stop_requested();
        bool empty_buf = (iovec_count == 0);

        ssize_t n             = 0;
        int     err           = 0;
        bool    have_sync_res = stop_now || empty_buf;
        if (!have_sync_res && spec_.may_speculate_write())
        {
            msghdr msg{};
            msg.msg_iov    = iovecs;
            msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(iovec_count);
            sockaddr_storage dest_copy = dest_storage;
            if (dest_len > 0)
            {
                msg.msg_name    = &dest_copy;
                msg.msg_namelen = dest_len;
            }
            int native_flags = to_native_msg_flags(flags) | MSG_NOSIGNAL;
            do { n = ::sendmsg(fd_, &msg, native_flags); }
            while (n < 0 && errno == EINTR);
            if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            {
                have_sync_res = true;
                if (n < 0) err = errno;
            }
            else
            {
                spec_.on_write_exhausted();
            }
        }

        if (have_sync_res)
        {
            if (sched_->try_consume_inline_budget())
            {
                if (ec)
                    *ec = stop_now ? capy::error::canceled
                          : err   ? make_err(err)
                                  : std::error_code{};
                if (bytes)
                    *bytes = (n < 0) ? 0u : static_cast<std::size_t>(n);
                send_.cont_op.cont.h = h;
                return dispatch_coro(ex, send_.cont_op.cont);
            }
            send_.prepare(h, ex, ec, bytes, fd_, sched_,
                shared_from_this(), &spec_, buffers, dest_len, dest_storage,
                to_native_msg_flags(flags), token);
            if (stop_now)
                send_.cancelled.store(true, std::memory_order_release);
            else
                send_.res = (n < 0) ? -err : static_cast<int>(n);
            sched_->work_started();
            {
                io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
                sched_->push_completed_locked(&send_);
            }
            return std::noop_coroutine();
        }

        send_.prepare(h, ex, ec, bytes, fd_, sched_, shared_from_this(),
            &spec_, buffers, dest_len, dest_storage,
            to_native_msg_flags(flags), token);
        sched_->work_started();
        if (send_.cancelled.load(std::memory_order_acquire))
        {
            io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
            sched_->push_completed_locked(&send_);
            return std::noop_coroutine();
        }
        io_uring_submit_op(*sched_, &send_);
        return std::noop_coroutine();
    }

    std::coroutine_handle<> submit_recv(
        std::coroutine_handle<>    h,
        capy::executor_ref         ex,
        buffer_param               buffers,
        bool                       want_source,
        corosio::local_endpoint*   source_out,
        int                        flags,
        std::stop_token            token,
        std::error_code*           ec,
        std::size_t*               bytes)
    {
        iovec iovecs[io_uring_max_iov];
        int   iovec_count = static_cast<int>(
            buffers.copy_to(
                reinterpret_cast<capy::mutable_buffer*>(iovecs),
                io_uring_max_iov));
        bool stop_now  = token.stop_possible() && token.stop_requested();
        bool empty_buf = (iovec_count == 0);

        ssize_t          n             = 0;
        int              err           = 0;
        bool             have_sync_res = stop_now || empty_buf;
        sockaddr_storage src_storage{};
        socklen_t        src_namelen   = 0;
        if (!have_sync_res && spec_.may_speculate_read())
        {
            msghdr msg{};
            msg.msg_iov    = iovecs;
            msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(iovec_count);
            if (want_source)
            {
                msg.msg_name    = &src_storage;
                msg.msg_namelen = sizeof(src_storage);
            }
            int native_flags = to_native_msg_flags(flags);
            do { n = ::recvmsg(fd_, &msg, native_flags); }
            while (n < 0 && errno == EINTR);
            if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            {
                have_sync_res = true;
                if (n < 0) err = errno;
                src_namelen = (n >= 0) ? msg.msg_namelen : 0;
            }
            else
            {
                spec_.on_read_exhausted();
            }
        }

        if (have_sync_res)
        {
            if (sched_->try_consume_inline_budget())
            {
                if (ec)
                    *ec = stop_now ? capy::error::canceled
                          : err   ? make_err(err)
                                  : std::error_code{};
                if (bytes)
                    *bytes = (n < 0) ? 0u : static_cast<std::size_t>(n);
                if (n >= 0 && want_source && source_out && !empty_buf)
                    *source_out = sockaddr_to_local_endpoint(src_storage, src_namelen);
                recv_.cont_op.cont.h = h;
                return dispatch_coro(ex, recv_.cont_op.cont);
            }
            recv_.prepare(h, ex, ec, bytes, fd_, sched_, shared_from_this(),
                &spec_, buffers, source_out,
                want_source ? &write_local_source : nullptr,
                to_native_msg_flags(flags), token);
            if (stop_now)
                recv_.cancelled.store(true, std::memory_order_release);
            else
            {
                recv_.res = (n < 0) ? -err : static_cast<int>(n);
                // Hand the speculative source over to do_handler's
                // source_writer so it translates into source_out the same
                // way the kernel-completed path would.
                if (n >= 0 && want_source)
                {
                    recv_.source_storage = src_storage;
                    recv_.source_len     = src_namelen;
                }
            }
            sched_->work_started();
            {
                io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
                sched_->push_completed_locked(&recv_);
            }
            return std::noop_coroutine();
        }

        recv_.prepare(h, ex, ec, bytes, fd_, sched_, shared_from_this(),
            &spec_, buffers, source_out,
            want_source ? &write_local_source : nullptr,
            to_native_msg_flags(flags), token);
        sched_->work_started();
        if (recv_.iovec_count == 0 ||
            recv_.cancelled.load(std::memory_order_acquire))
        {
            io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
            sched_->push_completed_locked(&recv_);
            return std::noop_coroutine();
        }
        io_uring_submit_op(*sched_, &recv_);
        return std::noop_coroutine();
    }

    static void write_local_source(
        void* ctx, sockaddr_storage const& s, socklen_t len) noexcept
    {
        if (auto* out = static_cast<corosio::local_endpoint*>(ctx))
            *out = sockaddr_to_local_endpoint(s, len);
    }
};

/** Unix domain datagram socket service for io_uring.

    Owns all `io_uring_local_datagram_socket` implementations for an
    `io_context`. Satisfies the `local_datagram_service` interface so the
    generic `local_datagram_socket` front-end can call `open_socket` and
    `bind_socket` transparently.

    Socket impls are reference-counted inside the service map; raw
    pointers returned from `construct()` remain valid until `destroy()`
    or `shutdown()` is called.

    @par Thread Safety
    All public member functions are thread-safe.
*/
class BOOST_COROSIO_DECL io_uring_local_datagram_service final
    : public local_datagram_service
{
public:
    /// Identifies this service for `execution_context` lookup.
    using key_type = local_datagram_service;

    /** Construct the local datagram service.

        @param ctx The owning execution context. The io_uring scheduler
            must already be registered.
    */
    explicit io_uring_local_datagram_service(capy::execution_context& ctx)
        : sched_(&ctx.use_service<io_uring_scheduler>())
    {}

    void shutdown() override
    {
        std::vector<std::shared_ptr<io_uring_local_datagram_socket>> live;
        {
            std::lock_guard lk(mutex_);
            live.reserve(impls_.size());
            for (auto& [_, p] : impls_)
                live.push_back(p);
        }
        // Cancel without the lock held to avoid inversion if cancel()
        // ever needs to re-enter the service.
        for (auto& p : live)
            p->cancel();
    }

    io_object::implementation* construct() override
    {
        auto p   = std::make_shared<io_uring_local_datagram_socket>(
            *this, *sched_);
        auto* raw = p.get();
        std::lock_guard lk(mutex_);
        impls_.emplace(raw, std::move(p));
        return raw;
    }

    void destroy(io_object::implementation* p) override
    {
        if (!p)
            return;
        std::lock_guard lk(mutex_);
        impls_.erase(static_cast<io_uring_local_datagram_socket*>(p));
    }

    // Close the fd eagerly when local_datagram_socket::close() is called,
    // before destroy() drops the shared_ptr and the destructor runs.
    void close(io_object::handle& h) override
    {
        auto* sock = static_cast<io_uring_local_datagram_socket*>(h.get());
        if (sock && sock->fd_ >= 0)
        {
            // Cancel pending SQEs before closing so the kernel resolves
            // the fd number while it is still valid.
            sched_->cancel_and_flush(sock->fd_);
            ::close(sock->fd_);
            sock->fd_              = -1;
            sock->local_endpoint_  = corosio::local_endpoint{};
            sock->remote_endpoint_ = corosio::local_endpoint{};
        }
    }

    /** Open an AF_UNIX datagram socket and associate it with an impl.

        Creates a non-blocking, close-on-exec socket via `socket(2)`.
        `family` is always `AF_UNIX` for local datagram sockets.

        @param impl     The socket implementation to initialise.
        @param family   Address family (`AF_UNIX`).
        @param type     Socket type (`SOCK_DGRAM`).
        @param protocol Protocol number (typically 0).
        @return Error code on failure, empty on success.
    */
    std::error_code open_socket(
        local_datagram_socket::implementation& impl,
        int family, int type, int protocol) override
    {
        auto& sock = static_cast<io_uring_local_datagram_socket&>(impl);
        int fd = ::socket(family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
        if (fd < 0)
            return make_err(errno);
        if (sock.fd_ >= 0)
        {
            sched_->submit_cancel_by_fd(sock.fd_);
            ::close(sock.fd_);
        }
        sock.fd_ = fd;
        return {};
    }

    /** Adopt a pre-created fd into an impl (e.g. from `socketpair`).

        Takes ownership of `fd` on success; the caller retains ownership
        on failure.

        @param impl The socket implementation to assign to.
        @param fd   A valid, open, non-blocking AF_UNIX datagram fd.
        @return Error code on failure, empty on success.
    */
    std::error_code assign_socket(
        local_datagram_socket::implementation& impl,
        native_handle_type fd) override
    {
        auto& sock = static_cast<io_uring_local_datagram_socket&>(impl);
        if (sock.fd_ >= 0)
        {
            sched_->cancel_and_flush(sock.fd_);
            ::close(sock.fd_);
        }
        sock.fd_ = static_cast<int>(fd);

        sockaddr_storage local{};
        socklen_t local_len = sizeof(local);
        if (::getsockname(sock.fd_,
                reinterpret_cast<sockaddr*>(&local), &local_len) == 0)
            sock.local_endpoint_ = sockaddr_to_local_endpoint(local, local_len);

        sockaddr_storage remote{};
        socklen_t remote_len = sizeof(remote);
        if (::getpeername(sock.fd_,
                reinterpret_cast<sockaddr*>(&remote), &remote_len) == 0)
            sock.remote_endpoint_ = sockaddr_to_local_endpoint(remote, remote_len);

        return {};
    }

    /** Bind the socket and capture the local endpoint via `getsockname`.

        @param impl The socket implementation to bind.
        @param ep   The local endpoint (path) to bind to.
        @return Error code on failure, empty on success.
    */
    std::error_code bind_socket(
        local_datagram_socket::implementation& impl,
        corosio::local_endpoint ep) override
    {
        auto& sock = static_cast<io_uring_local_datagram_socket&>(impl);
        sockaddr_storage addr{};
        socklen_t len = endpoint_to_sockaddr(ep, addr);
        if (::bind(
                sock.fd_,
                reinterpret_cast<sockaddr*>(&addr), len) < 0)
            return make_err(errno);

        sockaddr_storage local{};
        socklen_t local_len = sizeof(local);
        if (::getsockname(
                sock.fd_,
                reinterpret_cast<sockaddr*>(&local), &local_len) == 0)
            sock.local_endpoint_ = sockaddr_to_local_endpoint(local, local_len);
        return {};
    }

    /// Return the scheduler used by sockets created by this service.
    io_uring_scheduler& scheduler() noexcept { return *sched_; }

private:
    io_uring_scheduler*  sched_;
    std::mutex           mutex_;
    std::unordered_map<io_uring_local_datagram_socket*,
                       std::shared_ptr<io_uring_local_datagram_socket>> impls_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IO_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_TYPES_HPP
