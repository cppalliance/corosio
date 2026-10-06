//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_OVERLAPPED_HANDLE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_OVERLAPPED_HANDLE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/buffer_param.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/native/detail/coro_op_complete.hpp>
#include <boost/corosio/native/detail/iocp/win_completion_key.hpp>
#include <boost/corosio/native/detail/iocp/win_dissociate.hpp>
#include <boost/corosio/native/detail/iocp/win_mutex.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/native/detail/iocp/win_validate_handle.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/executor_ref.hpp>

#include <algorithm>
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <utility>
#include <vector>

/* The overlapped-handle core shared by stream_file, random_access_file,
   win_stream_handle and win_random_access_handle.

   Two op models sit on one handle base. The slot model embeds one read
   and one write op (one of each in flight) and either tracks a file
   position or submits at offset 0. The concurrent model allocates an op
   per call so any number of positional ops can be in flight.

   Each front keeps the shared-pointer "state + thin wrapper" layering:
   ops hold the state alive until their last packet drains, which can
   outlive the wrapper.
*/

namespace boost::corosio::detail {

/** Map a handle completion code to what decode_io_result expects.

    On reads, end of data (`ERROR_HANDLE_EOF`, `ERROR_BROKEN_PIPE`
    once a pipe's writer is gone, or `ERROR_PIPE_NOT_CONNECTED` after
    the server disconnects) becomes zero, so a zero-byte read
    decodes as eof, and `ERROR_MORE_DATA` (a partial message on a
    message-mode pipe) becomes a successful partial read. Socket ops
    never pass through here: on a datagram socket `ERROR_MORE_DATA`
    means truncation, which is an error.
*/
inline DWORD
normalize_handle_error(DWORD err, bool is_read) noexcept
{
    if (is_read &&
        (err == ERROR_HANDLE_EOF || err == ERROR_BROKEN_PIPE ||
         err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_MORE_DATA))
        return 0;
    return err;
}

/// A service's list of live states, walked by `shutdown()`.
template<class State>
class win_state_list
{
public:
    void add(State& s) noexcept
    {
        std::lock_guard<win_mutex> lock(mutex_);
        list_.push_back(&s);
    }

    void remove(State& s) noexcept
    {
        std::lock_guard<win_mutex> lock(mutex_);
        list_.remove(&s);
    }

    void close_all() noexcept
    {
        // A state whose last owner is mid-destruction cannot be pinned;
        // it is blocked in remove() on this mutex and closes itself.
        // Pins are released outside the lock, since dropping the last
        // one runs a destructor that calls remove().
        std::vector<std::shared_ptr<State>> pins;
        {
            std::lock_guard<win_mutex> lock(mutex_);
            for (auto* s = list_.pop_front(); s != nullptr;
                 s       = list_.pop_front())
                if (auto pin = s->weak_from_this().lock())
                    pins.push_back(std::move(pin));
        }
        for (auto& p : pins)
            p->close_handle();
    }

private:
    win_mutex mutex_;
    intrusive_list<State> list_;
};

/// A service's states and the wrappers it handed out.
template<class Wrapper, class State>
class win_handle_registry
{
public:
    win_handle_registry() = default;
    win_handle_registry(win_handle_registry const&) = delete;
    win_handle_registry& operator=(win_handle_registry const&) = delete;

    ~win_handle_registry()
    {
        for (auto* w = wrappers_.pop_front(); w != nullptr;
             w       = wrappers_.pop_front())
            delete w;
    }

    win_state_list<State>& states() noexcept
    {
        return states_;
    }

    Wrapper* add(Wrapper* w)
    {
        std::lock_guard<win_mutex> lock(mutex_);
        wrappers_.push_back(w);
        return w;
    }

    void destroy(Wrapper& w) noexcept
    {
        w.close_internal();
        {
            std::lock_guard<win_mutex> lock(mutex_);
            wrappers_.remove(&w);
        }
        delete &w;
    }

    void shutdown() noexcept
    {
        states_.close_all();
    }

private:
    win_state_list<State> states_;
    win_mutex mutex_;
    intrusive_list<Wrapper> wrappers_;
};

class win_handle_base;

/// An overlapped read or write on an adopted handle.
struct handle_io_op : overlapped_op
{
    void* buf              = nullptr;
    DWORD buf_len          = 0;
    win_handle_base* owner = nullptr;
    std::shared_ptr<win_handle_base> keep_alive;

    explicit handle_io_op(func_type f) noexcept;
    static void do_cancel_impl(overlapped_op* base) noexcept;
};

/** State shared by every overlapped handle front. */
class win_handle_base
    : public intrusive_list<win_handle_base>::node
    , public std::enable_shared_from_this<win_handle_base>
{
public:
    win_handle_base(
        win_scheduler& sched, win_state_list<win_handle_base>& list) noexcept
        : sched_(sched)
        , list_(list)
    {
        list_.add(*this);
    }

    ~win_handle_base()
    {
        list_.remove(*this);
    }

    win_handle_base(win_handle_base const&)            = delete;
    win_handle_base& operator=(win_handle_base const&) = delete;

    HANDLE native_handle() const noexcept
    {
        return handle_;
    }

    bool is_open() const noexcept
    {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    /// Close the handle; pending ops complete with ERROR_OPERATION_ABORTED.
    void close_handle() noexcept
    {
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            ::CancelIoEx(handle_, nullptr);
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

protected:
    /** Validate @p h and register it with this context's port.

        The object is closed; the public `assign()` rejects an open one.
    */
    std::error_code check_and_register(HANDLE h, handle_kind kind) noexcept
    {
        if (auto ec = validate_overlapped_handle(h, kind))
            return ec;
        if (!::CreateIoCompletionPort(
                h, static_cast<HANDLE>(sched_.native_handle()), key_io, 0))
        {
            DWORD const err = ::GetLastError();
            // Already bound to another port. Mapped here because the
            // system_category -> errc mapping of 87 differs by toolchain.
            if (err == ERROR_INVALID_PARAMETER)
                return std::make_error_code(std::errc::invalid_argument);
            return iocp_make_err(err, /*accept_path=*/false);
        }
        return {};
    }

    /** Unbind the handle from the port and give it up.

        Called after the ops were cancelled. Throws, leaving the object
        holding the handle, when an op is still in flight or Windows
        refuses the detach: detaching under a queued completion would
        lose it or deliver it to the next adopter's port. Asio fails
        its release() the same way.

        @param idle True when no op of this handle is in flight.
    */
    HANDLE detach_or_throw(bool idle, char const* what)
    {
        if (!idle)
            detail::throw_system_error(
                std::make_error_code(std::errc::device_or_resource_busy), what);
        if (!dissociate_from_iocp(handle_))
            detail::throw_system_error(
                std::make_error_code(std::errc::operation_not_supported), what);
        return std::exchange(handle_, INVALID_HANDLE_VALUE);
    }

    /** Issue ReadFile/WriteFile for an op the caller has set up.

        The caller has filled `h`, `ex`, `ec_out`, `bytes_out`,
        `is_read`, `owner` and `keep_alive`, called `start(token)` and
        `work_started()`.
    */
    void start_io(
        handle_io_op& op, std::uint64_t offset, buffer_param param) noexcept
    {
        static constexpr std::size_t max_buffers = 16;

        // Closed-object contract: bad_file_descriptor without the kernel.
        if (handle_ == INVALID_HANDLE_VALUE)
        {
            sched_.on_completion(&op, ERROR_INVALID_HANDLE, 0);
            return;
        }

        capy::mutable_buffer bufs[max_buffers];
        auto const count = param.copy_to(bufs, max_buffers);
        if (count == 0)
        {
            op.empty_buffer = true;
            sched_.on_completion(&op, 0, 0);
            return;
        }

        // ReadFile/WriteFile take one contiguous buffer.
        op.buf     = bufs[0].data();
        op.buf_len = static_cast<DWORD>(
            (std::min)(bufs[0].size(), std::size_t(0x7fffffff)));
        op.Offset     = static_cast<DWORD>(offset & 0xFFFFFFFF);
        op.OffsetHigh = static_cast<DWORD>(offset >> 32);

        BOOL const ok = op.is_read
            ? ::ReadFile(handle_, op.buf, op.buf_len, nullptr, &op)
            : ::WriteFile(handle_, op.buf, op.buf_len, nullptr, &op);
        DWORD const err = ok ? 0 : ::GetLastError();

        // ERROR_MORE_DATA completed a partial message and still queues
        // a packet, so it joins the pending path.
        if (err != 0 && err != ERROR_IO_PENDING && err != ERROR_MORE_DATA)
        {
            sched_.on_completion(&op, err, 0);
            return;
        }

        // Re-check cancellation now that the kernel I/O is issued. This
        // must precede on_pending: once it runs, another thread may
        // dispatch (and for concurrent ops delete) the op, so neither the
        // op nor this may be touched afterwards. Dispatch cannot happen
        // before on_pending, and a cancel arriving after this check
        // reaches the issued I/O through do_cancel_impl's own CancelIoEx.
        if (op.cancelled.load(std::memory_order_acquire))
            ::CancelIoEx(handle_, &op);

        sched_.on_pending(&op);
    }

    win_scheduler& sched_;
    win_state_list<win_handle_base>& list_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

inline handle_io_op::handle_io_op(func_type f) noexcept : overlapped_op(f)
{
    cancel_func_ = &do_cancel_impl;
}

inline void
handle_io_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<handle_io_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->owner && op->owner->is_open())
        ::CancelIoEx(op->owner->native_handle(), op);
}

/** One read and one write in flight: stream_file, win_stream_handle. */
class win_slot_handle : public win_handle_base
{
public:
    win_slot_handle(
        win_scheduler& sched,
        win_state_list<win_handle_base>& list,
        bool track_offset) noexcept
        : win_handle_base(sched, list)
        , track_offset_(track_offset)
    {
    }

    std::coroutine_handle<> read_some(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes_out)
    {
        return start(rd_, true, h, ex, param, std::move(token), ec, bytes_out);
    }

    std::coroutine_handle<> write_some(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes_out)
    {
        return start(wr_, false, h, ex, param, std::move(token), ec, bytes_out);
    }

    void cancel() noexcept
    {
        if (handle_ != INVALID_HANDLE_VALUE)
            ::CancelIoEx(handle_, nullptr);
        rd_.request_cancel();
        wr_.request_cancel();
    }

    std::error_code assign(native_handle_type nh, handle_kind kind) noexcept
    {
        HANDLE h = reinterpret_cast<HANDLE>(nh);
        if (auto ec = check_and_register(h, kind))
            return ec;
        generation_.fetch_add(1, std::memory_order_acq_rel);
        handle_ = h;
        offset_.store(0, std::memory_order_release);
        return {};
    }

    native_handle_type release()
    {
        // Cancel first, as asio does: a release that then fails still
        // aborts the in-flight I/O, and a retry once it drains succeeds.
        rd_.request_cancel();
        wr_.request_cancel();
        ::CancelIoEx(handle_, nullptr);
        bool const idle = !rd_.in_flight.load(std::memory_order_acquire) &&
            !wr_.in_flight.load(std::memory_order_acquire);
        HANDLE h = detach_or_throw(idle, "release");
        generation_.fetch_add(1, std::memory_order_acq_rel);
        offset_.store(0, std::memory_order_release);
        return reinterpret_cast<native_handle_type>(h);
    }

    /// Close the handle; completions still queued no longer move the position.
    void close_handle() noexcept
    {
        generation_.fetch_add(1, std::memory_order_acq_rel);
        win_handle_base::close_handle();
    }

protected:
    struct slot_op : handle_io_op
    {
        std::atomic<bool> in_flight{false};
        std::uint32_t generation = 0;

        slot_op() noexcept : handle_io_op(&do_complete) {}

        static void do_complete(
            void* owner,
            scheduler_op* base,
            std::uint32_t /*bytes*/,
            std::uint32_t /*error*/)
        {
            auto* op = static_cast<slot_op*>(base);
            op->in_flight.store(false, std::memory_order_release);
            if (!owner)
            {
                op->cleanup_only();
                op->keep_alive.reset();
                return;
            }

            auto* self = static_cast<win_slot_handle*>(op->owner);
            // A completion from before assign()/release() belongs to the
            // old handle; its byte count must not move the new position.
            if (self->track_offset_ && op->dwError == 0 &&
                op->bytes_transferred > 0 &&
                self->generation_.load(std::memory_order_acquire) ==
                    op->generation)
                self->offset_.fetch_add(
                    op->bytes_transferred, std::memory_order_acq_rel);

            op->dwError = normalize_handle_error(op->dwError, op->is_read);
            auto prevent_premature_destruction = std::move(op->keep_alive);
            op->invoke_handler();
        }
    };

    std::coroutine_handle<> start(
        slot_op& op,
        bool is_read,
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes_out)
    {
        op.keep_alive = shared_from_this();
        op.reset();
        op.owner     = this;
        op.is_read   = is_read;
        op.h         = h;
        op.ex        = ex;
        op.ec_out    = ec;
        op.bytes_out = bytes_out;
        op.start(token);

        sched_.work_started();
        op.generation = generation_.load(std::memory_order_acquire);
        op.in_flight.store(true, std::memory_order_release);
        start_io(
            op, track_offset_ ? offset_.load(std::memory_order_acquire) : 0,
            param);
        return std::noop_coroutine();
    }

    slot_op rd_;
    slot_op wr_;
    std::atomic<std::uint64_t> offset_{0};
    // Bumped by assign(), release() and close; tags each op.
    std::atomic<std::uint32_t> generation_{0};
    bool track_offset_;
};

/** Any number of positional ops: random_access_file, win_random_access_handle. */
class win_concurrent_handle : public win_handle_base
{
public:
    using win_handle_base::win_handle_base;

    std::coroutine_handle<> read_some_at(
        std::uint64_t offset,
        capy::continuation& cont,
        capy::executor_ref ex,
        buffer_param param,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes_out)
    {
        return start(
            true, offset, cont, ex, param, std::move(token), ec, bytes_out);
    }

    std::coroutine_handle<> write_some_at(
        std::uint64_t offset,
        capy::continuation& cont,
        capy::executor_ref ex,
        buffer_param param,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes_out)
    {
        return start(
            false, offset, cont, ex, param, std::move(token), ec, bytes_out);
    }

    void cancel() noexcept
    {
        if (handle_ != INVALID_HANDLE_VALUE)
            ::CancelIoEx(handle_, nullptr);
        request_cancel_all();
    }

    std::error_code assign(native_handle_type nh, handle_kind kind) noexcept
    {
        HANDLE h = reinterpret_cast<HANDLE>(nh);
        if (auto ec = check_and_register(h, kind))
            return ec;
        handle_ = h;
        return {};
    }

    native_handle_type release()
    {
        request_cancel_all();
        ::CancelIoEx(handle_, nullptr);
        bool idle;
        {
            std::lock_guard<win_mutex> lock(ops_mutex_);
            idle = outstanding_ops_.empty();
        }
        return reinterpret_cast<native_handle_type>(
            detach_or_throw(idle, "release"));
    }

protected:
    struct concurrent_op
        : handle_io_op
        , intrusive_list<concurrent_op>::node
    {
        capy::continuation* user_cont = nullptr;

        concurrent_op() noexcept : handle_io_op(&do_complete) {}

        // The continuation lives in the awaitable, so the op can go first.
        static void do_complete(
            void* owner,
            scheduler_op* base,
            std::uint32_t /*bytes*/,
            std::uint32_t /*error*/)
        {
            auto* op   = static_cast<concurrent_op*>(base);
            auto* self = static_cast<win_concurrent_handle*>(op->owner);
            auto keep  = std::move(op->keep_alive);
            op->stop_cb.reset();
            {
                std::lock_guard<win_mutex> lock(self->ops_mutex_);
                self->outstanding_ops_.remove(op);
            }

            if (!owner)
            {
                op->h = {};
                delete op;
                return;
            }

            DWORD const err = normalize_handle_error(op->dwError, op->is_read);
            decode_io_result(
                op->ec_out, op->bytes_out,
                op->cancelled.load(std::memory_order_acquire),
                err != 0 ? iocp_make_err(err, /*accept_path=*/false)
                         : std::error_code{},
                op->is_read, static_cast<std::size_t>(op->bytes_transferred),
                op->empty_buffer);

            capy::continuation* c = op->user_cont;
            c->h                  = op->h;
            capy::executor_ref ex = op->ex;
            delete op;
            dispatch_coro(ex, *c).resume();
        }
    };

    void request_cancel_all() noexcept
    {
        std::lock_guard<win_mutex> lock(ops_mutex_);
        outstanding_ops_.for_each(
            [](concurrent_op* op) { op->request_cancel(); });
    }

    std::coroutine_handle<> start(
        bool is_read,
        std::uint64_t offset,
        capy::continuation& cont,
        capy::executor_ref ex,
        buffer_param param,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes_out)
    {
        auto* op       = new concurrent_op();
        op->keep_alive = shared_from_this();
        op->reset();
        op->owner     = this;
        op->is_read   = is_read;
        op->user_cont = &cont;
        op->h         = cont.h;
        op->ex        = ex;
        op->ec_out    = ec;
        op->bytes_out = bytes_out;
        op->start(token);

        sched_.work_started();
        {
            std::lock_guard<win_mutex> lock(ops_mutex_);
            outstanding_ops_.push_back(op);
        }
        start_io(*op, offset, param);
        return std::noop_coroutine();
    }

    win_mutex ops_mutex_;
    intrusive_list<concurrent_op> outstanding_ops_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif
