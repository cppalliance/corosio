//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_MULTISHOT_ACCEPTOR_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_MULTISHOT_ACCEPTOR_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IO_URING

#include <liburing.h>

#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_acceptor_ops.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_buffer.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_op.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_scheduler.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_socket_ops.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/io/io_object.hpp>

#include <atomic>
#include <coroutine>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <system_error>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

template<class Derived, class ImplBase, class Endpoint, class PeerService>
class io_uring_multishot_acceptor_base
    : public ImplBase
    , public std::enable_shared_from_this<Derived>
{
protected:
    struct ready_fd_node : intrusive_list<ready_fd_node>::node
    {
        int               fd       = -1;
        sockaddr_storage  peer{};
        socklen_t         peer_len = 0;
    };

    struct waiter_node;

    struct waiter_canceller
    {
        waiter_node* w;
        void operator()() const noexcept;
    };

    struct waiter_node : intrusive_list<waiter_node>::node
    {
        std::coroutine_handle<>                              h;
        capy::executor_ref                                   ex;
        std::error_code*                                     ec_out   = nullptr;
        io_object::implementation**                          impl_out = nullptr;
        Derived*                                             owner    = nullptr;
        std::atomic<bool>                                    cancelled{false};
        std::optional<std::stop_callback<waiter_canceller>>  stop_cb;
    };

    int                                          fd_ = -1;
    io_uring_scheduler*                          sched_;
    PeerService*                                 peer_service_;
    Endpoint                                     local_endpoint_{};
    mutable std::mutex                           mutex_;
    intrusive_list<ready_fd_node>                ready_fds_;
    intrusive_list<waiter_node>                  waiters_;
    std::unique_ptr<uring_multi_accept_op>       multi_op_;
    bool                                         closing_ = false;

public:
    io_uring_multishot_acceptor_base(
        io_uring_scheduler& sched, PeerService& peer_svc) noexcept
        : sched_(&sched)
        , peer_service_(&peer_svc)
    {}

    ~io_uring_multishot_acceptor_base() override
    {
        {
            std::lock_guard lk(mutex_);
            closing_ = true;
        }
        if (fd_ >= 0)
        {
            sched_->submit_cancel_by_fd(fd_);
            // Drain parked fds — no waiter will consume them now.
            intrusive_list<ready_fd_node> drained;
            {
                std::lock_guard lk(mutex_);
                while (auto* r = ready_fds_.pop_front())
                    drained.push_back(r);
            }
            while (auto* r = drained.pop_front())
            {
                ::close(r->fd);
                delete r;
            }
            ::close(fd_);
            fd_ = -1;
        }

        // Break the multi_op_ → impl_ptr (shared_ptr<this>) cycle and
        // drain pending CQEs so unique_ptr<multi_op_> can free safely.
        if (multi_op_)
        {
            multi_op_->impl_ptr.reset();
            sched_->drain_cqes_for(multi_op_.get());
        }
    }

    Endpoint local_endpoint() const noexcept override
    {
        return local_endpoint_;
    }

    bool is_open() const noexcept override
    {
        return fd_ >= 0;
    }

    void cancel() noexcept override
    {
        drain_waiters_only();
        if (fd_ >= 0)
            sched_->submit_cancel_by_fd(fd_);
    }

    /// Drain queued waiters with operation_aborted but do NOT submit
    /// any kernel cancel for the fd. Used by service close() paths
    /// that have already submitted (or are about to submit) the
    /// cancel-by-fd themselves via `cancel_and_flush`.
    void drain_waiters_only() noexcept
    {
        intrusive_list<waiter_node> drained;
        {
            std::lock_guard lk(mutex_);
            closing_ = true;
            // Drain under the lock — the kernel cancel may not produce
            // a !more CQE before the fd is closed, so we can't rely on
            // on_accept_cqe_impl to surface operation_aborted.
            while (auto* w = waiters_.pop_front())
                drained.push_back(w);
        }

        while (auto* w = drained.pop_front())
        {
            w->stop_cb.reset();
            auto* op = new uring_accept_op();
            op->h        = w->h;
            op->ex       = w->ex;
            op->ec_out   = w->ec_out;
            op->impl_out = w->impl_out;
            op->cancelled.store(true, std::memory_order_release);
            delete w;
            sched_->post(op);
            sched_->work_finished();
        }
    }

    std::error_code set_option(
        int level, int optname,
        void const* data, std::size_t size) noexcept override
    {
        if (fd_ < 0) return make_err(EBADF);
        if (::setsockopt(fd_, level, optname,
                reinterpret_cast<char const*>(data),
                static_cast<socklen_t>(size)) < 0)
            return make_err(errno);
        return {};
    }

    std::error_code get_option(
        int level, int optname,
        void* data, std::size_t* size) const noexcept override
    {
        if (fd_ < 0) return make_err(EBADF);
        socklen_t len = static_cast<socklen_t>(*size);
        if (::getsockopt(fd_, level, optname,
                reinterpret_cast<char*>(data), &len) < 0)
            return make_err(errno);
        *size = static_cast<std::size_t>(len);
        return {};
    }

    void start_multishot()
    {
        if (!multi_op_)
        {
            multi_op_ = std::make_unique<uring_multi_accept_op>();
            multi_op_->listen_fd     = fd_;
            multi_op_->acceptor_impl = this;
            multi_op_->on_cqe        =
                &io_uring_multishot_acceptor_base::on_accept_cqe;
            multi_op_->impl_ptr      = this->shared_from_this();
        }
        else
        {
            // Reuse the existing op (re-arm path). Reset peer scratch
            // so the kernel writes into a clean slot.
            multi_op_->peer_storage = sockaddr_storage{};
            multi_op_->peer_len     = sizeof(sockaddr_storage);
        }

        auto* op = multi_op_.get();
        io_uring_submit_op(*sched_, op);
        // Deliberately no work_started(): the multishot SQE is a persistent
        // internal mechanism. User-visible work is tracked per-accept call.
    }

    /// Pull a parked fd or queue a waiter — used by Derived::accept().
    /// Either case ends with the calling coroutine suspending; the
    /// caller returns `std::noop_coroutine()` unconditionally.
    void dispatch_or_queue(
        std::coroutine_handle<>     h,
        capy::executor_ref          ex,
        std::stop_token             token,
        std::error_code*            ec,
        io_object::implementation** impl_out)
    {
        sockaddr_storage peer_storage{};
        socklen_t        peer_len = sizeof(peer_storage);
        int accepted_fd = ::accept4(fd_,
            reinterpret_cast<sockaddr*>(&peer_storage), &peer_len,
            SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (accepted_fd >= 0)
        {
            auto* op = new uring_accept_op();
            op->h            = h;
            op->ex           = ex;
            op->ec_out       = ec;
            op->impl_out     = impl_out;
            op->peer_service = peer_service_;
            op->adopt_fn     = &Derived::adopt_thunk;
            op->accepted_fd  = accepted_fd;
            op->peer_storage = peer_storage;
            op->peer_len     = peer_len;
            sched_->post(op);
            return;
        }
        // accept4 returned <0 — only EAGAIN/EWOULDBLOCK should fall
        // through to the parked/waiter path. Other errors (EBADF, etc.)
        // surface through the existing scheduler-completion path so the
        // user sees them via the op's ec_out. Build an op with `err`
        // set so do_handler delivers make_err(err).
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            int saved_errno = errno;
            auto* op = new uring_accept_op();
            op->h        = h;
            op->ex       = ex;
            op->ec_out   = ec;
            op->impl_out = impl_out;
            op->err      = saved_errno;
            sched_->post(op);
            return;
        }

        uring_accept_op* ready_op = nullptr;
        {
            std::lock_guard lk(mutex_);
            if (auto* r = ready_fds_.pop_front())
            {
                ready_op = new uring_accept_op();
                ready_op->h            = h;
                ready_op->ex           = ex;
                ready_op->ec_out       = ec;
                ready_op->impl_out     = impl_out;
                ready_op->peer_service = peer_service_;
                ready_op->adopt_fn     = &Derived::adopt_thunk;
                ready_op->accepted_fd  = r->fd;
                ready_op->peer_storage = r->peer;
                ready_op->peer_len     = r->peer_len;
                delete r;
            }
            else
            {
                auto* w = new waiter_node{};
                w->h        = h;
                w->ex       = ex;
                w->ec_out   = ec;
                w->impl_out = impl_out;
                w->owner    = static_cast<Derived*>(this);
                if (token.stop_possible())
                    w->stop_cb.emplace(token, waiter_canceller{w});
                sched_->work_started();
                waiters_.push_back(w);
                return;
            }
        }
        // Post outside the lock — acceptor mutex_ must never be held
        // while dispatch_mutex_ is acquired by sched_->post().
        sched_->post(ready_op);
    }

    void cancel_waiter(waiter_node* w) noexcept
    {
        {
            std::lock_guard lk(mutex_);
            if (closing_) return;  // on_accept_cqe_impl will drain with closing_ set
            waiters_.remove(w);
        }
        auto* op = new uring_accept_op();
        op->h        = w->h;
        op->ex       = w->ex;
        op->ec_out   = w->ec_out;
        op->impl_out = w->impl_out;
        op->cancelled.store(true, std::memory_order_release);
        delete w;
        // post() increments outstanding_work_; balances the work_started()
        // from accept() when the waiter was queued.
        sched_->post(op);
        sched_->work_finished();  // balance the work_started() from accept()
    }

private:
    static void on_accept_cqe(
        void* self_ptr, int new_fd, int err, bool more) noexcept
    {
        static_cast<Derived*>(self_ptr)
            ->on_accept_cqe_impl(new_fd, err, more);
    }

protected:
    void on_accept_cqe_impl(int new_fd, int err, bool more) noexcept
    {
        bool was_closing = false;
        waiter_node* matched = nullptr;
        intrusive_list<waiter_node> closing_waiters;
        {
            std::lock_guard lk(mutex_);
            was_closing = closing_;
            if (was_closing)
            {
                if (new_fd >= 0)
                    ::close(new_fd);
                if (!more)
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
                    auto* node     = new ready_fd_node{};
                    node->fd       = new_fd;
                    node->peer     = multi_op_->peer_storage;
                    node->peer_len = multi_op_->peer_len;
                    ready_fds_.push_back(node);
                }
            }
            else if (new_fd >= 0)
            {
                auto* node      = new ready_fd_node{};
                node->fd        = new_fd;
                node->peer      = multi_op_->peer_storage;
                node->peer_len  = multi_op_->peer_len;
                ready_fds_.push_back(node);
            }
        }

        if (matched)
        {
            matched->stop_cb.reset();
            auto* op         = new uring_accept_op();
            op->h            = matched->h;
            op->ex           = matched->ex;
            op->ec_out       = matched->ec_out;
            op->impl_out     = matched->impl_out;
            op->peer_service = peer_service_;
            op->adopt_fn     = &Derived::adopt_thunk;
            if (err)
            {
                op->err = err;
            }
            else if (new_fd >= 0)
            {
                op->accepted_fd  = new_fd;
                op->peer_storage = multi_op_->peer_storage;
                op->peer_len     = multi_op_->peer_len;
            }
            delete matched;
            sched_->post(op);
            sched_->work_finished();  // balance waiter's work_started
        }

        while (auto* w = closing_waiters.pop_front())
        {
            w->stop_cb.reset();
            auto* op = new uring_accept_op();
            op->h        = w->h;
            op->ex       = w->ex;
            op->ec_out   = w->ec_out;
            op->impl_out = w->impl_out;
            op->cancelled.store(true, std::memory_order_release);
            delete w;
            sched_->post(op);
            sched_->work_finished();  // balance waiter's work_started
        }

        if (!more && !was_closing)
        {
            // Re-arm: kernel terminated multishot non-fatally.
            struct rearm_op final : scheduler_op
            {
                std::shared_ptr<Derived> self_;
                explicit rearm_op(std::shared_ptr<Derived> s) noexcept
                    : self_(std::move(s)) {}

                void operator()() override
                {
                    auto self = std::move(self_);
                    delete this;
                    {
                        std::lock_guard lk(self->mutex_);
                        if (self->closing_)
                            return;
                    }
                    self->start_multishot();
                }

                void destroy() override { delete this; }
            };
            sched_->post(new rearm_op(this->shared_from_this()));
        }
    }
};

template<class Derived, class ImplBase, class Endpoint, class PeerService>
inline void
io_uring_multishot_acceptor_base<Derived, ImplBase, Endpoint, PeerService>
    ::waiter_canceller::operator()() const noexcept
{
    if (w->cancelled.exchange(true, std::memory_order_acq_rel))
        return;
    w->owner->cancel_waiter(w);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IO_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_MULTISHOT_ACCEPTOR_HPP
