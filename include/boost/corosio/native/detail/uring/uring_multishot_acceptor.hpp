//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_URING_URING_MULTISHOT_ACCEPTOR_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_URING_URING_MULTISHOT_ACCEPTOR_HPP

#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_URING

#include <liburing.h>

#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/uring/uring_acceptor_ops.hpp>
#include <boost/corosio/native/detail/uring/uring_buffer.hpp>
#include <boost/corosio/native/detail/uring/uring_op.hpp>
#include <boost/corosio/native/detail/uring/uring_scheduler.hpp>
#include <boost/corosio/native/detail/uring/uring_socket_ops.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/io/io_object.hpp>

#include <atomic>
#include <cstdint>
#include <coroutine>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <utility>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

/** Check whether a descriptor is in the listening state.

    Multishot accept fails immediately on a non-listening socket and
    the re-arm path would spin on that failure, so an adopted fd is
    only armed when the kernel reports a listener. A query the
    platform refuses is treated as listening so adoption still works.

    @param fd The descriptor to probe.
    @return True unless the kernel positively reports a non-listener.
*/
inline bool
fd_is_listening(int fd) noexcept
{
    int accepting  = 0;
    socklen_t alen = sizeof(accepting);
    if (::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &alen) != 0)
        return true;
    return accepting != 0;
}

template<
    class Derived,
    class ImplBase,
    class Endpoint,
    class PeerService,
    class AcceptorService>
class uring_multishot_acceptor_base
    : public ImplBase
    , public intrusive_list<Derived>::node
{
protected:
    struct ready_fd_node : intrusive_list<ready_fd_node>::node
    {
        int fd = -1;
        sockaddr_storage peer{};
        socklen_t peer_len = 0;
    };

    /** Pooled accept node: parked waiter and posted completion in one.

        A parked accept used to be a `waiter_node` whose fields were
        copied into a freshly allocated `uring_accept_op` at every
        completion boundary. The two had strictly sequential lifetimes
        (a waiter *becomes* a completion), so this node is both at
        once — the intrusive hook parks it in `waiters_`/`free_nodes_`
        and the inherited op posts it to the scheduler. Nodes recycle
        through the owning acceptor's `free_nodes_` via `dispose`, so
        steady-state accepts allocate nothing.

        The inherited `cancelled` flag is the claim token: delivery,
        arming-failure, and cancellation each claim the node with one
        `exchange`, and the loser leaves completion to the winner.
        Because the flag also decides how `do_handler` reports the
        completion, every claiming path stores the final status after
        `stop_cb.reset()` (which synchronizes with any in-flight
        canceller) and before posting.

        While in flight — parked or posted — the node holds an
        `object_ref` on its acceptor (the inherited `object_ref_`
        slot), so the impl cannot retire out from under `dispose`.
    */
    struct accept_node
        : uring_accept_op
        , intrusive_list<accept_node>::node
    {
        Derived* owner = nullptr;
        /// True once linked into `waiters_` (guarded by `mutex_`).
        /// The stop callback is armed before the node is queued, so
        /// cancel_waiter must not unlink a node it never queued.
        bool queued = false;
        /// A readiness wait rather than an accept: completion
        /// observes a pending connection without consuming it.
        bool peek = false;

        accept_node() noexcept
        {
            this->dispose = &recycle_thunk;
        }

        /// Claim the node for cancellation (the stop token fired);
        /// losing the exchange means a delivery already owns it.
        void on_cancel() noexcept override
        {
            if (this->cancelled.exchange(true, std::memory_order_acq_rel))
                return;
            owner->cancel_waiter(this);
        }

        static void recycle_thunk(uring_accept_op* op) noexcept
        {
            auto* n = static_cast<accept_node*>(op);
            n->owner->release_node(n);
        }
    };

    int fd_ = -1;
    uring_scheduler* sched_;
    PeerService* peer_service_;
    /// Owning acceptor service; exposes the recycling pool to
    /// retire(). Not used for anything else — the service already
    /// drives shutdown/destroy from its own side.
    AcceptorService* acceptor_svc_;
    Endpoint local_endpoint_{};
    mutable std::mutex mutex_;
    intrusive_list<ready_fd_node> ready_fds_;
    intrusive_list<accept_node> waiters_;
    /// Recycled accept nodes and ready-fd nodes (guarded by `mutex_`).
    /// Both survive impl recycling — the storage belongs to this impl
    /// and only the destructor frees it — so steady-state accepts
    /// allocate nothing.
    intrusive_list<accept_node> free_nodes_;
    intrusive_list<ready_fd_node> free_ready_;
    /// Single parked readiness wait (guarded by `mutex_`). Multishot
    /// accepting drains the kernel queue instantly, so a listener's
    /// readiness lives in `ready_fds_`, not in `poll()`; the wait is
    /// completed by the next delivery instead of a kernel poll.
    accept_node* read_wait_ = nullptr;
    /// Ended armings, kept for the next one (guarded by `mutex_`).
    /// Survives impl recycling like `free_nodes_`; entries hold no
    /// reference.
    intrusive_list<uring_multi_accept_op> free_ops_;
    /// The arming whose deliveries belong to this acceptor (guarded by
    /// `mutex_`). Identity only: the kernel owns the op and its
    /// reference. Never set while `closing_` is.
    uring_multi_accept_op* armed_ = nullptr;
    bool closing_ = false;
    /// Non-zero once an arming failed to reach the kernel (guarded by
    /// `mutex_`). Nothing will ever deliver a connection through an
    /// SQE the ring never took, so an accept reports this instead of
    /// parking on a delivery that cannot come. Cleared by the next
    /// arming that does reach the kernel.
    int arm_err_ = 0;

private:
    // CRTP ctor private + Derived friended so the base cannot be
    // constructed except as a CRTP base of Derived
    // (clang-tidy bugprone-crtp-constructor-accessibility).
    friend Derived;
    uring_multishot_acceptor_base(
        AcceptorService& acceptor_svc,
        uring_scheduler& sched,
        PeerService& peer_svc) noexcept
        : sched_(&sched)
        , peer_service_(&peer_svc)
        , acceptor_svc_(&acceptor_svc)
    {
    }

protected:
    /** Pop a recycled accept node, or allocate on the cold path.

        Resets every per-use field and arms the `object_ref` keepalive
        on this acceptor; the caller fills the request fields and
        either parks or posts the node. OOM on the cold path
        terminates, matching every other noexcept initiation path.
    */
    accept_node* acquire_node() noexcept
    {
        accept_node* n;
        {
            std::lock_guard lk(mutex_);
            n = free_nodes_.pop_front();
        }
        if (!n)
        {
            // NOLINTNEXTLINE(bugprone-unhandled-exception-at-new) — noexcept initiation path: OOM => std::terminate is the intended behavior
            n = new accept_node();
        }
        n->owner             = static_cast<Derived*>(this);
        n->queued            = false;
        n->peek              = false;
        n->err               = 0;
        n->accepted_fd       = -1;
        n->peer_len          = 0;
        n->ec_out            = nullptr;
        n->impl_out          = nullptr;
        n->peer_endpoint_out = nullptr;
        n->peer_service      = nullptr;
        n->adopt_fn          = nullptr;
        n->cancelled.store(false, std::memory_order_relaxed);
        n->object_ref_ = detail::object_ref(this);
        return n;
    }

    /** Return a consumed node to the free list; `dispose` target.

        The keepalive is moved out first and dropped after the lock,
        so a final release (which may reenter this impl's retire())
        never runs under `mutex_`.
    */
    void release_node(accept_node* n) noexcept
    {
        auto keep = std::move(n->object_ref_);
        {
            std::lock_guard lk(mutex_);
            free_nodes_.push_front(n);
        }
    }

    /// Pop a recycled ready-fd node, or allocate on the cold path.
    /// @pre `mutex_` is held.
    ready_fd_node* acquire_ready_locked() noexcept
    {
        if (auto* r = free_ready_.pop_front())
        {
            r->fd       = -1;
            r->peer_len = 0;
            return r;
        }
        // NOLINTNEXTLINE(bugprone-unhandled-exception-at-new) — CQE handler: noexcept, OOM => std::terminate is the intended behavior
        return new ready_fd_node{};
    }

public:
    /** Close and free every fd parked in `ready_fds_`.

        A multishot CQE can deliver a connection before any `accept()`
        is outstanding to claim it; that connection's fd (and tracking
        node) sits in `ready_fds_` until a later `accept()` consumes it
        or the acceptor goes away. Idempotent: safe to call from both
        `retire()` (recycling) and the destructor (pool-force-sweep
        backstop) — the second call always sees an empty list.
    */
    void drain_ready_fds() noexcept
    {
        intrusive_list<ready_fd_node> drained;
        {
            std::lock_guard lk(mutex_);
            while (auto* r = ready_fds_.pop_front())
                drained.push_back(r);
        }
        discard_ready(drained);
    }

    /// Close each parked connection in @p stale and return its node
    /// to `free_ready_`. @pre `mutex_` is not held.
    void discard_ready(intrusive_list<ready_fd_node>& stale) noexcept
    {
        if (stale.empty())
            return;
        stale.for_each([](ready_fd_node* r) { ::close(r->fd); });
        std::lock_guard lk(mutex_);
        while (auto* r = stale.pop_front())
            free_ready_.push_front(r);
    }

    /** Recycle into the owning service's pool at zero references.

        No arming can be live here: an armed op holds a reference.
    */
    void retire() noexcept override
    {
        drain_ready_fds();
        acceptor_svc_->pool_.recycle(static_cast<Derived*>(this));
    }

    ~uring_multishot_acceptor_base() override
    {
        uring_multi_accept_op* arming;
        {
            std::lock_guard lk(mutex_);
            closing_ = true;
            arming   = std::exchange(armed_, nullptr);
        }
        if (fd_ >= 0)
        {
            sched_->flush_after_cancel(fd_, [&]() noexcept {
                if (arming)
                    sched_->cancel_op_locked(arming);
            });
            ::close(fd_);
            fd_ = -1;
        }

        // Backstop: retire() already drains this on the normal
        // recycle path. This call only does real work when this impl
        // is reached by the pool's unconditional force-sweep at
        // shutdown without ever going through retire() first.
        drain_ready_fds();

        // The recycled-node storage belongs to this impl and is only
        // freed here; in-flight nodes hold an object_ref on this impl,
        // so none can still be parked or posted once the count reached
        // zero (or the pool's force-sweep ran after scheduler drain).
        // An arming the kernel never returned is not in `free_ops_`
        // and is deliberately leaked: the ring may still write to it.
        while (auto* n = free_nodes_.pop_front())
            delete n;
        while (auto* r = free_ready_.pop_front())
            delete r;
        while (auto* op = free_ops_.pop_front())
            delete op;
    }

    Endpoint local_endpoint() const noexcept override
    {
        return local_endpoint_;
    }

    bool is_open() const noexcept override
    {
        return fd_ >= 0;
    }

    native_handle_type native_handle() const noexcept override
    {
        return fd_;
    }

    corosio::family family() const noexcept override
    {
        return to_family(socket_family(fd_));
    }

    native_handle_type release_socket() noexcept override
    {
        // Mirror the service close() path, except that the caller now
        // owns the fd.
        if (fd_ >= 0)
            flush_listener();
        int fd          = fd_;
        fd_             = -1;
        local_endpoint_ = Endpoint{};
        return fd;
    }

    /// Abort every operation and stop accepting, for service teardown.
    void abort_all() noexcept
    {
        auto* arming = drain_waiters_only();
        static_cast<Derived*>(this)->cancel_wait_op();
        if (arming)
            sched_->cancel_ops({arming});
    }

    /** Stop accepting and flush every SQE naming the listener.

        Disowns the arming and cancels it with the readiness wait, so
        the caller may then close or hand off the descriptor.

        @pre The descriptor is open.
    */
    void flush_listener() noexcept
    {
        auto* arming = drain_waiters_only();
        static_cast<Derived*>(this)->cancel_wait_op();
        sched_->flush_after_cancel(fd_, [&]() noexcept {
            if (arming)
                sched_->cancel_op_locked(arming);
        });
    }

    /** Cancel the pending accepts and waits.

        The arming is left alone: the acceptor keeps listening, and a
        connection that arrives meanwhile waits for the next accept.
    */
    void cancel() noexcept override
    {
        intrusive_list<accept_node> claimed;
        {
            std::lock_guard lk(mutex_);
            // A waiter the stop callback already claimed belongs to it;
            // see fail_arm().
            intrusive_list<accept_node> keep;
            while (auto* w = waiters_.pop_front())
            {
                if (!w->cancelled.exchange(true, std::memory_order_acq_rel))
                    claimed.push_back(w);
                else
                    keep.push_back(w);
            }
            while (auto* w = keep.pop_front())
                waiters_.push_back(w);
            if (read_wait_ &&
                !read_wait_->cancelled.exchange(
                    true, std::memory_order_acq_rel))
            {
                claimed.push_back(read_wait_);
                read_wait_ = nullptr;
            }
        }
        while (auto* w = claimed.pop_front())
        {
            // The claim is also the completion status.
            w->stop_cb.reset();
            sched_->post(w);
            sched_->work_finished();
        }
        static_cast<Derived*>(this)->cancel_wait_op();
    }

    /** Reset state for recycling.

        `close()`/`release_socket()` already drove fd_, local_endpoint_,
        and the waiter/ready-fd queues to empty before the refcount
        reached zero, so those are asserted rather than re-cleared.
        `closing_` and `arm_err_` are the two fields that are NOT reset
        by that close path (closing_ is only ever cleared by a re-listen
        on a still-live acceptor, not by close) and must be explicitly
        reset here or the next session would see this impl as still
        shutting down.

        @pre refs_ == 0, fd closed, no op or waiter in flight.
    */
    void reuse() noexcept
    {
        BOOST_COROSIO_ASSERT(fd_ == -1);
        BOOST_COROSIO_ASSERT(local_endpoint_ == Endpoint{});
        BOOST_COROSIO_ASSERT(ready_fds_.empty());
        BOOST_COROSIO_ASSERT(waiters_.empty());
        BOOST_COROSIO_ASSERT(read_wait_ == nullptr);
        BOOST_COROSIO_ASSERT(armed_ == nullptr);
        closing_ = false;
        arm_err_ = 0;
    }

    /** Disown the arming and drain queued waiters with
        operation_aborted, without submitting a kernel cancel.

        Runs before the caller cancels the arming it returns: an arming
        admitted after this sees `closing_` and is refused, and the one
        admitted before it is already in the submission queue ahead of
        the cancel.

        @return The disowned arming, or null if none was live.
    */
    uring_multi_accept_op* drain_waiters_only() noexcept
    {
        intrusive_list<accept_node> drained;
        uring_multi_accept_op* arming;
        {
            std::lock_guard lk(mutex_);
            closing_ = true;
            arming   = std::exchange(armed_, nullptr);
            // Drain under the lock — the kernel cancel may not produce
            // a !more CQE before the fd is closed, so we can't rely on
            // on_accept_cqe_impl to surface operation_aborted.
            while (auto* w = waiters_.pop_front())
                drained.push_back(w);
            if (read_wait_)
            {
                drained.push_back(read_wait_);
                read_wait_ = nullptr;
            }
        }

        while (auto* w = drained.pop_front())
        {
            // reset() synchronizes with an in-flight canceller; after
            // it the stored status is ours to set.
            w->stop_cb.reset();
            w->cancelled.store(true, std::memory_order_release);
            sched_->post(w);
            sched_->work_finished();
        }
        return arming;
    }

    /** Park a readiness wait, or complete it if a connection is
        already queued.

        Multishot accepting consumes the kernel queue as connections
        arrive, so a poll on the listener never reports it readable;
        readiness is the impl's ready queue plus future deliveries.
    */
    void park_read_wait(
        capy::continuation& cont,
        capy::executor_ref ex,
        std::stop_token const& token,
        std::error_code* ec) noexcept
    {
        auto* w   = acquire_node();
        w->cont   = &cont;
        w->ex     = ex;
        w->ec_out = ec;
        w->peek   = true;

        bool ready   = false;
        bool aborted = false;
        {
            std::lock_guard lk(mutex_);
            if (closing_)
            {
                aborted = true;
            }
            else if (!ready_fds_.empty())
            {
                ready = true;
            }
        }
        if (ready || aborted)
        {
            if (aborted)
                w->cancelled.store(true, std::memory_order_release);
            sched_->post(w);
            return;
        }

        // Same protocol as accept parking: arm the callback before
        // the node is visible and outside `mutex_` (a pre-stopped
        // token invokes the canceller synchronously, and the
        // canceller takes `mutex_`).
        w->start(token);

        bool was_cancelled = false;
        int arm_err        = 0;
        {
            std::lock_guard lk(mutex_);
            if (w->cancelled.load(std::memory_order_acquire) || closing_)
            {
                was_cancelled = true;
            }
            else if (ready_fds_.empty())
            {
                // Readiness here means a future delivery, which a failed
                // arming has already ruled out: report it rather than
                // wait on one.
                arm_err = arm_err_;
                if (arm_err == 0)
                {
                    w->queued = true;
                    sched_->work_started();
                    read_wait_ = w;
                    return;
                }
            }
            // else: a connection arrived while the callback was armed;
            // complete as ready below.
        }

        w->stop_cb.reset();
        w->err = arm_err;
        w->cancelled.store(was_cancelled, std::memory_order_release);
        sched_->post(w);
    }

    std::error_code set_option(
        int level,
        int optname,
        void const* data,
        std::size_t size) noexcept override
    {
        if (fd_ < 0)
            return make_err(EBADF);
        if (::setsockopt(
                fd_, level, optname, reinterpret_cast<char const*>(data),
                static_cast<socklen_t>(size)) < 0)
            return make_err(errno);
        return {};
    }

    std::error_code
    get_option(int level, int optname, void* data, std::size_t* size)
        const noexcept override
    {
        if (fd_ < 0)
            return make_err(EBADF);
        socklen_t len = static_cast<socklen_t>(*size);
        if (::getsockopt(
                fd_, level, optname, reinterpret_cast<char*>(data), &len) < 0)
            return make_err(errno);
        *size = static_cast<std::size_t>(len);
        return {};
    }

    /** Take over an already-listening descriptor.

        Clears the shutdown latch a previous release left behind and
        discards connections parked from the replaced descriptor:
        those belong to the socket the caller is handing away.

        @pre No arming is live (`drain_waiters_only` has run if the
            acceptor had a descriptor).

        @param fd The adopted descriptor.
    */
    void adopt_listening_fd(int fd) noexcept
    {
        intrusive_list<ready_fd_node> stale;
        {
            std::lock_guard lk(mutex_);
            BOOST_COROSIO_ASSERT(armed_ == nullptr);
            fd_      = fd;
            closing_ = false;
            while (auto* r = ready_fds_.pop_front())
                stale.push_back(r);
        }
        discard_ready(stale);
    }

    /** Ready the acceptor for a listen-time arming.

        Returns `false` when a live arming already covers the
        descriptor: a re-listen only changes the backlog. Otherwise
        clears the shutdown latch a released descriptor left behind and
        discards the connections it delivered, which belong to the
        socket the caller took away.
    */
    bool prepare_listen_arm() noexcept
    {
        intrusive_list<ready_fd_node> stale;
        {
            std::lock_guard lk(mutex_);
            if (armed_)
                return false;
            closing_ = false;
            while (auto* r = ready_fds_.pop_front())
                stale.push_back(r);
        }
        discard_ready(stale);
        return true;
    }

    /** Arm a multishot accept on the current descriptor.

        The op is handed to the kernel with a reference on this
        acceptor; the caller must hold one. Refused when closing or
        when an arming is already live.
    */
    void start_multishot() noexcept
    {
        uring_multi_accept_op* op;
        {
            std::lock_guard lk(mutex_);
            op = free_ops_.pop_front();
        }
        if (!op)
        {
            // NOLINTNEXTLINE(bugprone-unhandled-exception-at-new) — noexcept initiation path: OOM => std::terminate is the intended behavior
            op                = new uring_multi_accept_op();
            op->sched_        = sched_;
            op->acceptor_impl = this;
            op->on_cqe        = &uring_multishot_acceptor_base::on_accept_cqe;
            op->on_end        = &uring_multishot_acceptor_base::on_arming_end;
        }
        op->object_ref_ = detail::object_ref(this);
        submit_arming(op, [this, op]() noexcept {
            if (closing_ || armed_)
                return false;
            armed_        = op;
            op->listen_fd = fd_;
            return true;
        });
    }

    /** Report an arming that never reached the kernel.

        No CQE can arrive for an SQE the ring never took, so an accept
        parked on this acceptor would park for good. The error is
        remembered for the accepts still to come and delivered now to
        whoever is already parked, which is the same EAGAIN every other
        op reports when the SQ stays full.

        @param err The code to report to the accepts this arming can no
            longer serve.
    */
    void fail_arm(int err) noexcept
    {
        intrusive_list<accept_node> claimed;
        {
            std::lock_guard lk(mutex_);
            arm_err_ = err;
            // Claim each waiter the way a delivery does. One the
            // canceller already claimed belongs to it: cancel_waiter
            // is waiting on this mutex to unlink the node itself, so
            // it has to still be in the list when it gets in.
            intrusive_list<accept_node> keep;
            while (auto* w = waiters_.pop_front())
            {
                if (!w->cancelled.exchange(true, std::memory_order_acq_rel))
                    claimed.push_back(w);
                else
                    keep.push_back(w);
            }
            while (auto* w = keep.pop_front())
                waiters_.push_back(w);
            if (read_wait_ &&
                !read_wait_->cancelled.exchange(
                    true, std::memory_order_acq_rel))
            {
                claimed.push_back(read_wait_);
                read_wait_ = nullptr;
            }
        }

        while (auto* w = claimed.pop_front())
        {
            // The exchange above was a claim, not the completion
            // status: after reset() quiesces the canceller, restore
            // the flag so the handler reports `err`, not canceled.
            w->stop_cb.reset();
            w->err = err;
            w->cancelled.store(false, std::memory_order_release);
            sched_->post(w);
            sched_->work_finished(); // balance the waiter's work_started
        }
    }

    /// Pull a parked fd or queue a waiter — used by Derived::accept().
    /// Either case ends with the calling coroutine suspending; the
    /// caller returns `std::noop_coroutine()` unconditionally.
    void dispatch_or_queue(
        capy::continuation& cont,
        capy::executor_ref ex,
        std::stop_token const& token,
        std::error_code* ec,
        io_object::implementation** impl_out)
    {
        auto* w     = acquire_node();
        w->cont     = &cont;
        w->ex       = ex;
        w->ec_out   = ec;
        w->impl_out = impl_out;

        sockaddr_storage peer_storage{};
        socklen_t peer_len = sizeof(peer_storage);
        int accepted_fd    = ::accept4(
            fd_, reinterpret_cast<sockaddr*>(&peer_storage), &peer_len,
            SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (accepted_fd >= 0)
        {
            w->peer_service = peer_service_;
            w->adopt_fn     = &Derived::adopt_thunk;
            w->accepted_fd  = accepted_fd;
            w->peer_storage = peer_storage;
            w->peer_len     = peer_len;
            sched_->post(w);
            return;
        }
        // accept4 returned <0 — only EAGAIN/EWOULDBLOCK should fall
        // through to the parked/waiter path. Other errors (EBADF, etc.)
        // surface through the existing scheduler-completion path so the
        // user sees them via the node's ec_out: `err` set means
        // do_handler delivers make_err(err).
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            w->err = errno;
            sched_->post(w);
            return;
        }

        bool have_ready = false;
        {
            std::lock_guard lk(mutex_);
            if (auto* r = ready_fds_.pop_front())
            {
                w->peer_service = peer_service_;
                w->adopt_fn     = &Derived::adopt_thunk;
                w->accepted_fd  = r->fd;
                w->peer_storage = r->peer;
                w->peer_len     = r->peer_len;
                free_ready_.push_front(r);
                have_ready = true;
            }
        }
        if (have_ready)
        {
            // Post outside the lock — acceptor mutex_ must never be
            // held while dispatch_mutex_ is acquired by sched_->post().
            sched_->post(w);
            return;
        }

        // Arm the stop callback before the node is visible in
        // `waiters_` and outside `mutex_`: an already-stopped token
        // invokes the canceller synchronously from emplace, and
        // cancel_waiter takes `mutex_` (self-deadlock if held).
        // Arming pre-queue also keeps the CQE handler from claiming
        // a node whose callback is not yet constructed.
        w->start(token);

        bool was_cancelled = false;
        {
            std::lock_guard lk(mutex_);
            if (w->cancelled.load(std::memory_order_acquire))
            {
                // Canceller already fired (pre-stopped token); it saw
                // queued == false and left completion to us.
                was_cancelled = true;
            }
            else if (auto* r = ready_fds_.pop_front())
            {
                // A connection arrived while the callback was armed;
                // prefer it over parking the waiter behind it.
                w->peer_service = peer_service_;
                w->adopt_fn     = &Derived::adopt_thunk;
                w->accepted_fd  = r->fd;
                w->peer_storage = r->peer;
                w->peer_len     = r->peer_len;
                free_ready_.push_front(r);
            }
            else if (arm_err_ != 0)
            {
                // No arming reached the kernel, so no CQE will deliver
                // a connection: parking here would park for good.
                w->err = arm_err_;
            }
            else
            {
                w->queued = true;
                sched_->work_started();
                waiters_.push_back(w);
                return;
            }
        }

        w->stop_cb.reset();
        w->cancelled.store(was_cancelled, std::memory_order_release);
        sched_->post(w);
    }

    void cancel_waiter(accept_node* w) noexcept
    {
        {
            std::lock_guard lk(mutex_);
            if (closing_)
                return; // drain_waiters_only will complete with closing_ set
            if (!w->queued)
                return; // not queued yet; the parking path observes
                        // `cancelled` and completes the node
            if (w->peek)
            {
                if (read_wait_ != w)
                    return; // already claimed by a delivery
                read_wait_ = nullptr;
            }
            else
            {
                waiters_.remove(w);
            }
            sched_->cancel_posting_begin();
        }
        // `cancelled` is already true (the claim that routed here);
        // that is also the completion status, so post as-is. The node's
        // stop_cb is still engaged — do_handler resets it, and running
        // that reset from the handler thread while this callback
        // returns is the stop_callback destructor's documented
        // wait-or-self case.
        sched_->post(w);
        sched_->work_finished(); // balance the work_started() from accept()
        sched_->cancel_posting_end();
    }

private:
    /** Submit @p op as the live arming, or recycle it.

        @param admit Runs under `mutex_`, itself under the ring mutex;
            claims `armed_` for @p op and returns true, or returns
            false to refuse.
    */
    template<class Admit>
    void submit_arming(uring_multi_accept_op* op, Admit admit) noexcept
    {
        op->peer_storage = sockaddr_storage{};
        op->peer_len     = sizeof(sockaddr_storage);
        op->res          = 0;
        // Deliberately no work_started(): the arming is an internal
        // mechanism; user-visible work is tracked per accept.
        // Once submitted, a re-arm's only reference to this acceptor
        // is the op's, which the kernel may already have ended, so
        // the error is cleared while admission still holds the lock.
        auto r = uring_try_submit_op_if(*sched_, op, [&]() noexcept {
            std::lock_guard lk(mutex_);
            if (!admit())
                return false;
            arm_err_ = 0;
            return true;
        });
        if (r == uring_guarded_submit::submitted)
            return;
        if (r == uring_guarded_submit::sq_full)
        {
            {
                std::lock_guard lk(mutex_);
                if (armed_ == op)
                    armed_ = nullptr;
            }
            fail_arm(EAGAIN);
        }
        recycle_arming(op);
    }

    /// Return an ended arming to `free_ops_`, then drop its reference,
    /// which may retire this impl, after the lock.
    void recycle_arming(uring_multi_accept_op* op) noexcept
    {
        auto keep = std::move(op->object_ref_);
        std::lock_guard lk(mutex_);
        free_ops_.push_front(op);
    }

    static void on_accept_cqe(
        void* self_ptr,
        uring_multi_accept_op* op,
        int new_fd,
        int err,
        bool more) noexcept
    {
        static_cast<Derived*>(self_ptr)->on_accept_cqe_impl(
            op, new_fd, err, more);
    }

    static void
    on_arming_end(void* self_ptr, uring_multi_accept_op* op, bool live) noexcept
    {
        auto* self = static_cast<Derived*>(self_ptr);
        if (live)
        {
            bool rearm;
            {
                std::lock_guard lk(self->mutex_);
                rearm = self->armed_ == op && !self->closing_;
            }
            if (rearm)
            {
                // The kernel ended a live arming on its own; keep
                // accepting with the same op and reference.
                self->submit_arming(op, [self, op]() noexcept {
                    return self->armed_ == op && !self->closing_;
                });
                return;
            }
        }
        {
            std::lock_guard lk(self->mutex_);
            if (self->armed_ == op)
                self->armed_ = nullptr;
        }
        self->recycle_arming(op);
    }

protected:
    void on_accept_cqe_impl(
        uring_multi_accept_op* op, int new_fd, int err, bool more) noexcept
    {
        accept_node* matched      = nullptr;
        accept_node* claimed_peek = nullptr;
        intrusive_list<accept_node> closing_waiters;
        {
            std::lock_guard lk(mutex_);
            // A disowned arming's deliveries belong to a descriptor
            // this acceptor no longer has.
            bool const stale = (op != armed_);
            if (!stale && new_fd >= 0 && read_wait_ &&
                !read_wait_->cancelled.exchange(
                    true, std::memory_order_acq_rel))
            {
                // A parked readiness wait observes the delivery
                // without consuming it; the connection still flows
                // to a waiter or the ready queue below.
                claimed_peek = read_wait_;
                read_wait_   = nullptr;
            }
            if (stale)
            {
                if (new_fd >= 0)
                    ::close(new_fd);
                if (!more && closing_)
                {
                    // Collect waiters to drain after the lock is released.
                    while (auto* w = waiters_.pop_front())
                        closing_waiters.push_back(w);
                }
            }
            else if (!waiters_.empty())
            {
                // Claim the head waiter atomically. If the canceller
                // already won the race (cancelled was already true),
                // leave the waiter in the list for cancel_waiter to
                // remove and dispatch with operation_aborted; park the
                // new_fd so the next waiter consumes it.
                auto* head_w = waiters_.front();
                if (!head_w->cancelled.exchange(
                        true, std::memory_order_acq_rel))
                {
                    waiters_.pop_front();
                    matched = head_w;
                }
                else if (new_fd >= 0)
                {
                    auto* node     = acquire_ready_locked();
                    node->fd       = new_fd;
                    node->peer     = op->peer_storage;
                    node->peer_len = op->peer_len;
                    ready_fds_.push_back(node);
                }
            }
            else if (new_fd >= 0)
            {
                auto* node     = acquire_ready_locked();
                node->fd       = new_fd;
                node->peer     = op->peer_storage;
                node->peer_len = op->peer_len;
                ready_fds_.push_back(node);
            }
        }

        // Each claim's exchange set `cancelled`; after reset() has
        // quiesced any in-flight canceller, restore the flag to the
        // real completion status before posting.
        if (claimed_peek)
        {
            claimed_peek->stop_cb.reset();
            claimed_peek->cancelled.store(false, std::memory_order_release);
            sched_->post(claimed_peek);
            sched_->work_finished(); // balance the parking work_started
        }

        if (matched)
        {
            matched->stop_cb.reset();
            matched->peer_service = peer_service_;
            matched->adopt_fn     = &Derived::adopt_thunk;
            if (err)
            {
                matched->err = err;
            }
            else if (new_fd >= 0)
            {
                matched->accepted_fd  = new_fd;
                matched->peer_storage = op->peer_storage;
                matched->peer_len     = op->peer_len;
            }
            matched->cancelled.store(false, std::memory_order_release);
            sched_->post(matched);
            sched_->work_finished(); // balance waiter's work_started
        }

        while (auto* w = closing_waiters.pop_front())
        {
            w->stop_cb.reset();
            w->cancelled.store(true, std::memory_order_release);
            sched_->post(w);
            sched_->work_finished(); // balance waiter's work_started
        }
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_URING_URING_MULTISHOT_ACCEPTOR_HPP
