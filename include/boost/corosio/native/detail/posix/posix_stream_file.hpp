//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_STREAM_FILE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_STREAM_FILE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/stream_file.hpp>
#include <boost/corosio/file_base.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/corosio/detail/continuation_op.hpp>
#include <boost/corosio/detail/thread_pool.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/corosio/detail/buffer_param.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/buffers.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <system_error>

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

/*
    POSIX Stream File Implementation
    =================================

    Regular files cannot be monitored by epoll/kqueue/select — the kernel
    always reports them as ready. Blocking I/O (pread/pwrite) is dispatched
    to a shared thread pool, with completion posted back to the scheduler.

    This follows the same pattern as posix_resolver: pool_work_item for
    dispatch, scheduler_op for completion, shared_from_this for lifetime.

    Completion Flow
    ---------------
    1. read_some() sets up file_read_op, posts to thread pool
    2. Pool thread runs preadv() (blocking)
    3. Pool thread stores results, posts scheduler_op to scheduler
    4. Scheduler invokes op() which resumes the coroutine

    Single-Inflight Constraint
    --------------------------
    Only one asynchronous operation may be in flight at a time on a
    given file object. Concurrent read and write is not supported
    because both share offset_ without synchronization.
*/

namespace boost::corosio::detail {

struct scheduler;
class posix_stream_file_service;

/** Stream file implementation for POSIX backends.

    Each instance contains embedded operation objects (read_op_, write_op_)
    that are reused across calls. This avoids per-operation heap allocation.
*/
class posix_stream_file final
    : public stream_file::implementation
    , public std::enable_shared_from_this<posix_stream_file>
    , public intrusive_list<posix_stream_file>::node
{
    friend class posix_stream_file_service;

public:
    static constexpr std::size_t max_buffers = 16;

    /** Operation state for a single file read or write. */
    struct file_op : scheduler_op
    {
        struct canceller
        {
            file_op* op;
            void operator()() const noexcept
            {
                op->request_cancel();
            }
        };

        // Coroutine state
        std::coroutine_handle<> h;
        detail::continuation_op cont_op;
        capy::executor_ref ex;

        // Output pointers
        std::error_code* ec_out = nullptr;
        std::size_t* bytes_out  = nullptr;

        // Buffer data (copied from buffer_param at submission time)
        iovec iovecs[max_buffers];
        int iovec_count = 0;

        // Result storage (populated by worker thread)
        int errn                    = 0;
        std::size_t bytes_transferred = 0;
        bool is_read                = false;

        // Thread coordination
        std::atomic<bool> cancelled{false};
        std::optional<std::stop_callback<canceller>> stop_cb;

        /// Prevents use-after-free when file is closed with pending ops.
        std::shared_ptr<void> impl_ref;

        file_op() = default;

        void reset() noexcept
        {
            iovec_count       = 0;
            errn              = 0;
            bytes_transferred = 0;
            is_read           = false;
            cancelled.store(false, std::memory_order_relaxed);
            stop_cb.reset();
            impl_ref.reset();
            ec_out    = nullptr;
            bytes_out = nullptr;
        }

        void operator()() override;
        void destroy() override;

        void request_cancel() noexcept
        {
            cancelled.store(true, std::memory_order_release);
        }

        void start(std::stop_token const& token)
        {
            cancelled.store(false, std::memory_order_release);
            stop_cb.reset();
            if (token.stop_possible())
                stop_cb.emplace(token, canceller{this});
        }
    };

    /** Pool work item for thread pool dispatch. */
    struct pool_op : pool_work_item
    {
        posix_stream_file* file_ = nullptr;
        std::shared_ptr<posix_stream_file> ref_;
    };

    explicit posix_stream_file(posix_stream_file_service& svc) noexcept;

    // -- io_stream::implementation --

    std::coroutine_handle<> read_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    std::coroutine_handle<> write_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    // -- stream_file::implementation --

    native_handle_type native_handle() const noexcept override
    {
        return fd_;
    }

    void cancel() noexcept override
    {
        read_op_.request_cancel();
        write_op_.request_cancel();
    }

    std::uint64_t size() const override;
    void resize(std::uint64_t new_size) override;
    void sync_data() override;
    void sync_all() override;
    native_handle_type release() override;
    void assign(native_handle_type handle) override;
    std::uint64_t seek(std::int64_t offset, file_base::seek_basis origin) override;

    // -- Internal --

    /** Open the file and store the fd. */
    std::error_code open_file(
        std::filesystem::path const& path, file_base::flags mode);

    /** Close the file descriptor. */
    void close_file() noexcept;

private:
    posix_stream_file_service& svc_;
    int fd_ = -1;
    std::uint64_t offset_ = 0;

    file_op read_op_;
    file_op write_op_;
    pool_op read_pool_op_;
    pool_op write_pool_op_;

    static void do_read_work(pool_work_item*) noexcept;
    static void do_write_work(pool_work_item*) noexcept;
};

// ---------------------------------------------------------------------------
// Inline implementation
// ---------------------------------------------------------------------------

inline
posix_stream_file::posix_stream_file(posix_stream_file_service& svc) noexcept
    : svc_(svc)
{
}

inline std::error_code
posix_stream_file::open_file(
    std::filesystem::path const& path, file_base::flags mode)
{
    close_file();

    int oflags = 0;

    // Access mode
    unsigned access = static_cast<unsigned>(mode) & 3u;
    if (access == static_cast<unsigned>(file_base::read_write))
        oflags |= O_RDWR;
    else if (access == static_cast<unsigned>(file_base::write_only))
        oflags |= O_WRONLY;
    else
        oflags |= O_RDONLY;

    // Creation flags
    if ((mode & file_base::create) != file_base::flags(0))
        oflags |= O_CREAT;
    if ((mode & file_base::exclusive) != file_base::flags(0))
        oflags |= O_EXCL;
    if ((mode & file_base::truncate) != file_base::flags(0))
        oflags |= O_TRUNC;
    if ((mode & file_base::append) != file_base::flags(0))
        oflags |= O_APPEND;
    if ((mode & file_base::sync_all_on_write) != file_base::flags(0))
        oflags |= O_SYNC;

    int fd = ::open(path.c_str(), oflags, 0666);
    if (fd < 0)
        return make_err(errno);

    fd_     = fd;
    offset_ = 0;

    // Append mode: position at end-of-file (preadv/pwritev use
    // explicit offsets, so O_APPEND alone is not sufficient).
    if ((mode & file_base::append) != file_base::flags(0))
    {
        struct stat st;
        if (::fstat(fd, &st) < 0)
        {
            int err = errno;
            ::close(fd);
            fd_ = -1;
            return make_err(err);
        }
        offset_ = static_cast<std::uint64_t>(st.st_size);
    }

#ifdef POSIX_FADV_SEQUENTIAL
    ::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    return {};
}

inline void
posix_stream_file::close_file() noexcept
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

inline std::uint64_t
posix_stream_file::size() const
{
    struct stat st;
    if (::fstat(fd_, &st) < 0)
        throw_system_error(make_err(errno), "stream_file::size");
    return static_cast<std::uint64_t>(st.st_size);
}

inline void
posix_stream_file::resize(std::uint64_t new_size)
{
    if (new_size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        throw_system_error(make_err(EOVERFLOW), "stream_file::resize");
    if (::ftruncate(fd_, static_cast<off_t>(new_size)) < 0)
        throw_system_error(make_err(errno), "stream_file::resize");
}

inline void
posix_stream_file::sync_data()
{
#if BOOST_COROSIO_HAS_POSIX_SYNCHRONIZED_IO
    if (::fdatasync(fd_) < 0)
#else // BOOST_COROSIO_HAS_POSIX_SYNCHRONIZED_IO
    if (::fsync(fd_) < 0)
#endif // BOOST_COROSIO_HAS_POSIX_SYNCHRONIZED_IO
        throw_system_error(make_err(errno), "stream_file::sync_data");
}

inline void
posix_stream_file::sync_all()
{
    if (::fsync(fd_) < 0)
        throw_system_error(make_err(errno), "stream_file::sync_all");
}

inline native_handle_type
posix_stream_file::release()
{
    int fd = fd_;
    fd_ = -1;
    offset_ = 0;
    return fd;
}

inline void
posix_stream_file::assign(native_handle_type handle)
{
    close_file();
    fd_ = handle;
    offset_ = 0;
}

inline std::uint64_t
posix_stream_file::seek(std::int64_t offset, file_base::seek_basis origin)
{
    // We track offset_ ourselves (not the kernel fd offset)
    // because preadv/pwritev use explicit offsets.
    std::int64_t new_pos;

    if (origin == file_base::seek_set)
    {
        new_pos = offset;
    }
    else if (origin == file_base::seek_cur)
    {
        new_pos = static_cast<std::int64_t>(offset_) + offset;
    }
    else
    {
        struct stat st;
        if (::fstat(fd_, &st) < 0)
            throw_system_error(make_err(errno), "stream_file::seek");
        new_pos = st.st_size + offset;
    }

    if (new_pos < 0)
        throw_system_error(make_err(EINVAL), "stream_file::seek");
    if (new_pos > static_cast<std::int64_t>(std::numeric_limits<off_t>::max()))
        throw_system_error(make_err(EOVERFLOW), "stream_file::seek");

    offset_ = static_cast<std::uint64_t>(new_pos);

    return offset_;
}

// -- file_op completion handler --
// (read_some, write_some, do_read_work, do_write_work are
//  defined in posix_stream_file_service.hpp after the service)

inline void
posix_stream_file::file_op::operator()()
{
    stop_cb.reset();

    bool const was_cancelled = cancelled.load(std::memory_order_acquire);

    if (ec_out)
    {
        if (was_cancelled)
            *ec_out = capy::error::canceled;
        else if (errn != 0)
            *ec_out = make_err(errn);
        else if (is_read && bytes_transferred == 0)
            *ec_out = capy::error::eof;
        else
            *ec_out = {};
    }

    if (bytes_out)
        *bytes_out = was_cancelled ? 0 : bytes_transferred;

    // Move impl_ref to a local so members remain valid through
    // dispatch — impl_ref may be the last shared_ptr keeping
    // the parent posix_stream_file (which embeds this file_op) alive.
    auto prevent_destroy = std::move(impl_ref);
    ex.on_work_finished();
    cont_op.cont.h = h;
    dispatch_coro(ex, cont_op.cont).resume();
}

inline void
posix_stream_file::file_op::destroy()
{
    stop_cb.reset();
    auto local_ex = ex;
    impl_ref.reset();
    local_ex.on_work_finished();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_STREAM_FILE_HPP
