//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_FILE_OPS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_FILE_OPS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IO_URING

#include <boost/corosio/native/detail/io_uring/io_uring_op.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_socket_ops.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>

#include <cstdint>
#include <sys/uio.h>

namespace boost::corosio::detail {

/** Scatter-gather file read via `IORING_OP_READV`.

    Stream files pass `offset == -1` so the kernel uses (and updates)
    the fd's `f_pos`, matching POSIX `read(2)` semantics. Random-
    access files pass an explicit caller-supplied offset.

    @par Handler dispatch
    `do_cqe` captures `res`/`cqe_flags` and queues self into `local`;
    `do_handler` runs from the scheduler queue and resumes the
    coroutine.
*/
/// Shared state and submission logic for file read ops. Concrete
/// subclasses pick a `do_handler` that matches their storage model:
/// `uring_file_read_op` for embedded slots (stream_file), and
/// `uring_random_access_read_op` for heap-allocated per-call ops
/// (random_access_file, where concurrent reads at different offsets
/// are legitimate).
struct uring_file_read_op_base : io_uring_op
{
    iovec        iovecs[io_uring_max_iov];
    int          iovec_count = 0;
    int          fd          = -1;
    std::int64_t offset      = -1;  // -1 means kernel f_pos

protected:
    explicit uring_file_read_op_base(func_type handler) noexcept
        : io_uring_op(handler, &do_cqe, &do_prep)
    {
        is_read = true;
    }

public:
    /** Reset and initialize for a new submission.

        @param file_offset -1 selects the kernel's `f_pos` (POSIX
        `read(2)` semantics for stream files); otherwise the explicit
        offset for random-access files.
    */
    void prepare(
        std::coroutine_handle<>  handle,
        capy::executor_ref       executor,
        std::error_code*         ec,
        std::size_t*             bytes,
        int                      file_descriptor,
        std::int64_t             file_offset,
        io_uring_scheduler*      scheduler,
        std::shared_ptr<void>    impl,
        buffer_param             buffers,
        std::stop_token const&   token) noexcept
    {
        h         = handle;
        ex        = executor;
        ec_out    = ec;
        bytes_out = bytes;
        fd        = file_descriptor;
        offset    = file_offset;
        sched_    = scheduler;
        impl_ptr  = std::move(impl);
        res       = 0;
        cqe_flags = 0;
        iovec_count = static_cast<int>(
            buffers.copy_to(
                reinterpret_cast<capy::mutable_buffer*>(iovecs),
                io_uring_max_iov));
        empty_buffer = (iovec_count == 0);
        start(token);
    }

    static void do_prep(io_uring_op* base, ::io_uring_sqe* sqe) noexcept
    {
        auto* self = static_cast<uring_file_read_op_base*>(base);
        ::io_uring_prep_readv(
            sqe, self->fd, self->iovecs, self->iovec_count,
            static_cast<__u64>(self->offset));
    }

    static void do_cqe(
        io_uring_op* base, int res, unsigned flags,
        op_queue& local) noexcept
    {
        auto* self      = static_cast<uring_file_read_op_base*>(base);
        self->res       = res;
        self->cqe_flags = flags;
        local.push(self);
    }

    /// Common post-completion work used by both handlers: fill ec_out
    /// and bytes_out, then return the coroutine to resume.
    static std::coroutine_handle<>
    finish(uring_file_read_op_base* self) noexcept
    {
        uring_set_result(self, /*is_read=*/true, self->empty_buffer);
        if (self->bytes_out)
            *self->bytes_out =
                self->res >= 0 ? static_cast<std::size_t>(self->res) : 0u;
        self->cont_op.cont.h = self->h;
        return dispatch_coro(self->ex, self->cont_op.cont);
    }
};

/// Scatter-gather file read embedded as a member of stream_file
/// (single-pending per fd). Handler uses the suicide-move pattern;
/// the impl owns this slot.
struct uring_file_read_op : uring_file_read_op_base
{
    uring_file_read_op() noexcept
        : uring_file_read_op_base(&do_handler) {}

    static void do_handler(
        void* owner, scheduler_op* base,
        std::uint32_t /*bytes*/, std::uint32_t /*error*/) noexcept
    {
        auto* self = static_cast<uring_file_read_op*>(base);
        self->stop_cb.reset();

        if (owner == nullptr)
        {
            auto suicide = std::move(self->impl_ptr);
            return;
        }

        auto next = finish(self);
        auto suicide = std::move(self->impl_ptr);
        next.resume();
    }
};

/// Heap-allocated scatter-gather file read for random_access_file —
/// each `read_some_at` call allocates a fresh op so multiple reads
/// at different offsets on the same fd can be in flight concurrently.
struct uring_random_access_read_op : uring_file_read_op_base
{
    uring_random_access_read_op() noexcept
        : uring_file_read_op_base(&do_handler) {}

    static void do_handler(
        void* owner, scheduler_op* base,
        std::uint32_t /*bytes*/, std::uint32_t /*error*/) noexcept
    {
        auto* self = static_cast<uring_random_access_read_op*>(base);
        self->stop_cb.reset();

        if (owner == nullptr)
        {
            delete self;
            return;
        }

        auto next = finish(self);
        delete self;
        next.resume();
    }
};

/** Scatter-gather file write via `IORING_OP_WRITEV`.

    Stream files pass `offset == -1` (kernel f_pos); random-access
    files pass an explicit caller-supplied offset. Unlike socket
    writes, no `MSG_NOSIGNAL` is needed — files don't generate
    SIGPIPE on closed peers.
*/
/// Shared state and submission logic for file write ops. Concrete
/// subclasses pick a `do_handler` matching their storage model.
struct uring_file_write_op_base : io_uring_op
{
    iovec        iovecs[io_uring_max_iov];
    int          iovec_count = 0;
    int          fd          = -1;
    std::int64_t offset      = -1;

protected:
    explicit uring_file_write_op_base(func_type handler) noexcept
        : io_uring_op(handler, &do_cqe, &do_prep) {}

public:
    /** Reset and initialize for a new submission.

        See uring_file_read_op_base::prepare for the offset convention.
    */
    void prepare(
        std::coroutine_handle<>  handle,
        capy::executor_ref       executor,
        std::error_code*         ec,
        std::size_t*             bytes,
        int                      file_descriptor,
        std::int64_t             file_offset,
        io_uring_scheduler*      scheduler,
        std::shared_ptr<void>    impl,
        buffer_param             buffers,
        std::stop_token const&   token) noexcept
    {
        h         = handle;
        ex        = executor;
        ec_out    = ec;
        bytes_out = bytes;
        fd        = file_descriptor;
        offset    = file_offset;
        sched_    = scheduler;
        impl_ptr  = std::move(impl);
        res       = 0;
        cqe_flags = 0;
        iovec_count = static_cast<int>(
            buffers.copy_to(
                reinterpret_cast<capy::mutable_buffer*>(iovecs),
                io_uring_max_iov));
        empty_buffer = (iovec_count == 0);
        start(token);
    }

    static void do_prep(io_uring_op* base, ::io_uring_sqe* sqe) noexcept
    {
        auto* self = static_cast<uring_file_write_op_base*>(base);
        ::io_uring_prep_writev(
            sqe, self->fd, self->iovecs, self->iovec_count,
            static_cast<__u64>(self->offset));
    }

    static void do_cqe(
        io_uring_op* base, int res, unsigned flags,
        op_queue& local) noexcept
    {
        auto* self      = static_cast<uring_file_write_op_base*>(base);
        self->res       = res;
        self->cqe_flags = flags;
        local.push(self);
    }

    static std::coroutine_handle<>
    finish(uring_file_write_op_base* self) noexcept
    {
        uring_set_result(self, /*is_read=*/false, self->empty_buffer);
        if (self->bytes_out)
            *self->bytes_out =
                self->res >= 0 ? static_cast<std::size_t>(self->res) : 0u;
        self->cont_op.cont.h = self->h;
        return dispatch_coro(self->ex, self->cont_op.cont);
    }
};

/// Embedded file write op for stream_file.
struct uring_file_write_op : uring_file_write_op_base
{
    uring_file_write_op() noexcept
        : uring_file_write_op_base(&do_handler) {}

    static void do_handler(
        void* owner, scheduler_op* base,
        std::uint32_t /*bytes*/, std::uint32_t /*error*/) noexcept
    {
        auto* self = static_cast<uring_file_write_op*>(base);
        self->stop_cb.reset();

        if (owner == nullptr)
        {
            auto suicide = std::move(self->impl_ptr);
            return;
        }

        auto next = finish(self);
        auto suicide = std::move(self->impl_ptr);
        next.resume();
    }
};

/// Heap-allocated file write op for random_access_file.
struct uring_random_access_write_op : uring_file_write_op_base
{
    uring_random_access_write_op() noexcept
        : uring_file_write_op_base(&do_handler) {}

    static void do_handler(
        void* owner, scheduler_op* base,
        std::uint32_t /*bytes*/, std::uint32_t /*error*/) noexcept
    {
        auto* self = static_cast<uring_random_access_write_op*>(base);
        self->stop_cb.reset();

        if (owner == nullptr)
        {
            delete self;
            return;
        }

        auto next = finish(self);
        delete self;
        next.resume();
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IO_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_FILE_OPS_HPP
