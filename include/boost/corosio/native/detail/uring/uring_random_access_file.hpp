//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_URING_URING_RANDOM_ACCESS_FILE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_URING_URING_RANDOM_ACCESS_FILE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_URING

#include <boost/corosio/detail/random_access_file_service.hpp>
#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/uring/uring_file_ops.hpp>
#include <boost/corosio/native/detail/uring/uring_file_service_base.hpp>
#include <boost/corosio/native/detail/uring/uring_scheduler.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/posix/large_file.hpp>
#include <boost/corosio/native/detail/validate_fd.hpp>
#include <boost/corosio/random_access_file.hpp>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <system_error>
#include <unordered_map>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace boost::corosio::detail {

class uring_random_access_file_service;

/** Native io_uring random-access-file implementation.

    Async `read_some_at` / `write_some_at` submit `IORING_OP_READV`
    / `IORING_OP_WRITEV` with the caller-supplied offset. Metadata
    operations (open, size, resize, sync, close) are synchronous
    syscalls.

    @par Thread Safety
    Concurrent `read_some_at` / `write_some_at` calls on the same
    file at distinct offsets are safe; ordering between two
    submissions at the same offset is unspecified at the kernel
    level (matches POSIX `pread(2)` / `pwrite(2)` semantics).
*/
class BOOST_COROSIO_DECL uring_random_access_file final
    : public random_access_file::implementation
    , public intrusive_list<uring_random_access_file>::node
{
    friend class uring_random_access_file_service;

    int fd_                               = -1;
    uring_scheduler* sched_               = nullptr;
    uring_random_access_file_service* svc_ = nullptr;

    // Random-access files legitimately support concurrent ops at
    // different offsets on the same fd (e.g. parallel reads in
    // testConcurrentReads). Embedding a single slot would smash
    // state across calls; ops recycle through per-file free lists
    // so concurrency is unbounded but steady state allocates
    // nothing. The storage belongs to this impl (it survives
    // recycling) and only the destructor frees it.

    /** Pooled random-access op: completion recycles instead of
        deleting.

        Reuses the heap variant's submission/finish machinery but
        swaps the completion handler (the func-pointer convention
        makes that one protected assignment) for one that returns the
        op to its file's free list. The inherited `object_ref_`
        keepalive is dropped only after the push, so a final release
        never runs under `ops_mutex_`.
    */
    template<class Base>
    struct pooled_op final
        : Base
        , intrusive_list<pooled_op<Base>>::node
    {
        uring_random_access_file* file_ = nullptr;

        pooled_op() noexcept
        {
            this->func_ = &do_pooled_handler;
        }

        static void do_pooled_handler(
            void* owner,
            scheduler_op* base,
            std::uint32_t /*bytes*/,
            std::uint32_t /*error*/) noexcept
        {
            auto* self = static_cast<pooled_op*>(base);
            self->stop_cb.reset();
            auto keep = std::move(self->object_ref_);
            if (owner == nullptr)
            {
                self->file_->release_op(self);
                return;
            }
            Base::finish(self);
            auto* c = self->cont;
            auto ex = self->ex;
            self->file_->release_op(self);
            dispatch_coro(ex, *c).resume();
        }
    };

    using pooled_read_op  = pooled_op<uring_random_access_read_op>;
    using pooled_write_op = pooled_op<uring_random_access_write_op>;

    std::mutex ops_mutex_;
    intrusive_list<pooled_read_op> free_reads_;
    intrusive_list<pooled_write_op> free_writes_;

    /// Pop a recycled op of the given kind, or allocate on the cold
    /// path; `prepare()` resets every per-use field.
    template<class Op>
    Op* acquire_op(intrusive_list<Op>& free_list)
    {
        Op* op;
        {
            std::lock_guard<std::mutex> lock(ops_mutex_);
            op = free_list.pop_front();
        }
        if (!op)
            op = new Op();
        op->file_ = this;
        return op;
    }

    void release_op(pooled_read_op* op) noexcept
    {
        std::lock_guard<std::mutex> lock(ops_mutex_);
        free_reads_.push_front(op);
    }

    void release_op(pooled_write_op* op) noexcept
    {
        std::lock_guard<std::mutex> lock(ops_mutex_);
        free_writes_.push_front(op);
    }

public:
    explicit uring_random_access_file(
        uring_random_access_file_service& svc,
        uring_scheduler& sched) noexcept
        : sched_(&sched)
        , svc_(&svc)
    {
    }

    ~uring_random_access_file() override
    {
        close_file();
        while (auto* op = free_reads_.pop_front())
            delete op;
        while (auto* op = free_writes_.pop_front())
            delete op;
    }

    /// Recycle into the owning service's pool. Defined out-of-line
    /// after uring_random_access_file_service for its complete type.
    void retire() noexcept override;

    /** Reset for recycling.

        `close_file()` already drove fd_ to its closed value before the
        refcount reached zero; every op holds its own `object_ref`
        while in flight, so none can be outstanding here. The free
        lists deliberately survive recycling — the storage belongs to
        this impl and only the destructor frees it.

        @pre refs_ == 0, fd closed, no op in flight.
    */
    void reuse() noexcept
    {
        BOOST_COROSIO_ASSERT(fd_ == -1);
    }

    // -- random_access_file::implementation --

    std::coroutine_handle<> read_some_at(
        std::uint64_t,
        capy::continuation&,
        capy::executor_ref,
        buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    std::coroutine_handle<> write_some_at(
        std::uint64_t,
        capy::continuation&,
        capy::executor_ref,
        buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    native_handle_type native_handle() const noexcept override
    {
        return fd_;
    }

    void cancel() noexcept override
    {
        if (fd_ >= 0)
            sched_->submit_cancel_by_fd(fd_);
    }

    std::uint64_t size() const override
    {
        file_stat_t st;
        if (file_fstat(fd_, &st) < 0)
            throw_system_error(make_err(errno), "random_access_file::size");
        return static_cast<std::uint64_t>(st.st_size);
    }

    std::error_code resize(std::uint64_t new_size) noexcept override
    {
        if (new_size > static_cast<std::uint64_t>(
                           (std::numeric_limits<file_off_t>::max)()))
            return make_err(EOVERFLOW);
        if (file_ftruncate(fd_, static_cast<file_off_t>(new_size)) < 0)
            return make_err(errno);
        return {};
    }

    std::error_code sync_data() noexcept override
    {
#if BOOST_COROSIO_HAS_POSIX_SYNCHRONIZED_IO
        if (::fdatasync(fd_) < 0)
#else
        if (::fsync(fd_) < 0)
#endif
            return make_err(errno);
        return {};
    }

    std::error_code sync_all() noexcept override
    {
        if (::fsync(fd_) < 0)
            return make_err(errno);
        return {};
    }

    native_handle_type release() override
    {
        // Flush the cancel while the fd is still open, so the kernel
        // resolves it before the caller can close and recycle the
        // number.
        if (fd_ >= 0)
            sched_->cancel_and_flush(fd_);
        int fd = fd_;
        fd_    = -1;
        return fd;
    }

    std::error_code assign(native_handle_type handle) noexcept override
    {
        // The public assign() guarantees the object is closed.
        if (auto ec = validate_file_fd(handle))
            return ec;

        fd_ = handle;
        return {};
    }

    // -- Internal --

    /// Open the file. Synchronous; sets `fd_`. Caller is the service.
    std::error_code
    open_file(std::filesystem::path const& path, file_base::flags mode)
    {
        close_file();

        int oflags      = 0;
        unsigned access = static_cast<unsigned>(mode) & 3u;
        if (access == static_cast<unsigned>(file_base::read_write))
            oflags |= O_RDWR;
        else if (access == static_cast<unsigned>(file_base::write_only))
            oflags |= O_WRONLY;
        else
            oflags |= O_RDONLY;

        if ((mode & file_base::create) != file_base::flags(0))
            oflags |= O_CREAT;
        if ((mode & file_base::exclusive) != file_base::flags(0))
            oflags |= O_EXCL;
        if ((mode & file_base::truncate) != file_base::flags(0))
            oflags |= O_TRUNC;
        if ((mode & file_base::sync_all_on_write) != file_base::flags(0))
            oflags |= O_SYNC;

        oflags |= O_CLOEXEC;

        int fd = ::open(path.c_str(), oflags | large_file_open_flag, 0666);
        if (fd < 0)
            return make_err(errno);

        fd_ = fd;

#ifdef POSIX_FADV_RANDOM
        // Hint the page cache that access will be random; matches
        // the POSIX backend.
        ::posix_fadvise(fd_, 0, 0, POSIX_FADV_RANDOM);
#endif

        return {};
    }

    /// Cancel any in-flight ops and close the fd. Idempotent.
    void close_file() noexcept
    {
        if (fd_ >= 0)
        {
            // The kernel may run a queued pipe write as task work at
            // either kernel entry below; with the reader already gone
            // that raises SIGPIPE.
            scoped_sigpipe_block no_sigpipe;
            sched_->cancel_and_flush(fd_);
            ::close(fd_);
            fd_ = -1;
        }
    }
};

inline std::coroutine_handle<>
uring_random_access_file::read_some_at(
    std::uint64_t user_offset,
    capy::continuation& cont,
    capy::executor_ref ex,
    buffer_param buffers,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes)
{
    auto* op = acquire_op(free_reads_);
    op->prepare(
        cont, ex, ec, bytes, fd_, static_cast<std::int64_t>(user_offset),
        sched_, detail::object_ref(this), buffers, token);
    sched_->work_started();

    // Closed-object contract outranks the zero-length no-op.
    if (fd_ < 0)
    {
        op->empty_buffer = false;
        op->res          = -EBADF;
        uring_scheduler::lock_type lock(sched_->dispatch_mutex());
        sched_->push_completed_locked(op);
        return std::noop_coroutine();
    }

    if (op->empty_buffer ||
        op->cancelled.load(std::memory_order_acquire))
    {
        uring_scheduler::lock_type lock(sched_->dispatch_mutex());
        sched_->push_completed_locked(op);
        return std::noop_coroutine();
    }

    uring_submit_op(*sched_, op);
    return std::noop_coroutine();
}

inline std::coroutine_handle<>
uring_random_access_file::write_some_at(
    std::uint64_t user_offset,
    capy::continuation& cont,
    capy::executor_ref ex,
    buffer_param buffers,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes)
{
    auto* op = acquire_op(free_writes_);
    op->prepare(
        cont, ex, ec, bytes, fd_, static_cast<std::int64_t>(user_offset),
        sched_, detail::object_ref(this), buffers, token);
    sched_->work_started();

    // Closed-object contract outranks the zero-length no-op.
    if (fd_ < 0)
    {
        op->empty_buffer = false;
        op->res          = -EBADF;
        uring_scheduler::lock_type lock(sched_->dispatch_mutex());
        sched_->push_completed_locked(op);
        return std::noop_coroutine();
    }

    if (op->empty_buffer ||
        op->cancelled.load(std::memory_order_acquire))
    {
        uring_scheduler::lock_type lock(sched_->dispatch_mutex());
        sched_->push_completed_locked(op);
        return std::noop_coroutine();
    }

    uring_submit_op(*sched_, op);
    return std::noop_coroutine();
}

/** Native io_uring random-access-file service.

    Owns all `uring_random_access_file` impls. Replaces
    `posix_random_access_file_service` for the io_uring backend;
    registered under the abstract `random_access_file_service` key
    by `uring_t::construct`.
*/
class BOOST_COROSIO_DECL uring_random_access_file_service final
    : public uring_file_service_base<
          uring_random_access_file_service,
          random_access_file_service,
          uring_random_access_file>
{
    using base_service = uring_file_service_base<
        uring_random_access_file_service,
        random_access_file_service,
        uring_random_access_file>;

public:
    explicit uring_random_access_file_service(
        capy::execution_context& ctx)
        : base_service(ctx.use_service<uring_scheduler>())
    {
    }

    // construct / destroy / close / shutdown / scheduler() are inherited
    // from uring_file_service_base.

    std::error_code open_file(
        random_access_file::implementation& impl,
        std::filesystem::path const& path,
        file_base::flags mode) override
    {
        return static_cast<uring_random_access_file&>(impl).open_file(
            path, mode);
    }
};

inline void
uring_random_access_file::retire() noexcept
{
    svc_->pool_.recycle(this);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_URING_URING_RANDOM_ACCESS_FILE_HPP
