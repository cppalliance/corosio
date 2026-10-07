//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_RANDOM_ACCESS_FILE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_RANDOM_ACCESS_FILE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/random_access_file.hpp>
#include <boost/corosio/file_base.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/corosio/detail/thread_pool.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/corosio/detail/buffer_param.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/native/detail/coro_op.hpp>
#include <boost/corosio/native/detail/coro_op_complete.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/posix/large_file.hpp>
#include <boost/corosio/native/detail/validate_fd.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/buffers.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <system_error>

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

/*
    POSIX Random-Access File Implementation
    ========================================

    Each async read/write acquires an raf_op that serves as both the
    thread-pool work item and the scheduler completion op. This
    allows unlimited concurrent operations on the same file object.

    Ops recycle through the file's free_ops_ list on completion or
    shutdown, so steady-state reads and writes allocate nothing; the
    file destructor frees the recycled storage.
*/

namespace boost::corosio::detail {

struct scheduler;
class posix_random_access_file_service;

/** Random-access file implementation for POSIX backends. */
class posix_random_access_file final
    : public random_access_file::implementation
    , public intrusive_list<posix_random_access_file>::node
{
    friend class posix_random_access_file_service;

public:
    static constexpr std::size_t max_buffers = 16;

    /** Per-operation state, acquired from the file's free list.

        Inherits from `coro_op` (for scheduler completion plus the shared
        coroutine, cancellation and keepalive machinery) and
        `pool_work_item` (for thread-pool dispatch). The intrusive hook
        links it into `outstanding_ops_` while in flight and `free_ops_`
        once recycled — membership is strictly sequential, so one hook
        serves both under `ops_mutex_`. `coro_op` leads the base list so
        a `scheduler_op*` round-trips.
    */
    struct raf_op final
        : coro_op
        , pool_work_item
        , intrusive_list<raf_op>::node
    {
        iovec iovecs[max_buffers];
        int iovec_count      = 0;
        std::uint64_t offset = 0;
        // Snapshotted at submission: assign() may replace fd_ while the
        // worker runs.
        int fd = -1;

        int errn                      = 0;
        std::size_t bytes_transferred = 0;

        // Raw back-pointer for the typed work; `object_ref_` is the keepalive.
        posix_random_access_file* file_ = nullptr;

        // The awaitable's, not the embedded `cont`: this op is freed
        // before the coroutine resumes.
        capy::continuation* awaiting = nullptr;

        void operator()() override;
        void destroy() override;

        /// Thread-pool work function: executes preadv/pwritev.
        static void do_work(pool_work_item*) noexcept;
    };

    explicit posix_random_access_file(
        posix_random_access_file_service& svc) noexcept;

    /// Recycle into the owning service's pool. Defined out-of-line
    /// after posix_random_access_file_service for its complete type.
    void retire() noexcept override;

    /** Reset for recycling.

        `close_file()` already drove fd_ to its closed value before
        the refcount reached zero. Every `raf_op` holds its own
        `object_ref` while in flight, so the refcount cannot reach
        zero while one is outstanding — asserting `outstanding_ops_`
        is empty is therefore a precondition check, not a defensive
        one. `free_ops_` deliberately survives recycling: the storage
        belongs to this impl and only the destructor frees it.

        @pre refs_ == 0, fd closed, no op in flight.
    */
    void reuse() noexcept
    {
        BOOST_COROSIO_ASSERT(fd_ == -1);
        BOOST_COROSIO_ASSERT(outstanding_ops_.empty());
    }

    ~posix_random_access_file() override
    {
        while (auto* op = free_ops_.pop_front())
            delete op;
    }

    // -- random_access_file::implementation --

    std::coroutine_handle<> read_some_at(
        std::uint64_t offset,
        capy::continuation&,
        capy::executor_ref,
        buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    std::coroutine_handle<> write_some_at(
        std::uint64_t offset,
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
        std::lock_guard<std::mutex> lock(ops_mutex_);
        outstanding_ops_.for_each([](raf_op* op) {
            op->cancelled.store(true, std::memory_order_release);
        });
    }

    std::uint64_t size() const override;
    std::error_code resize(std::uint64_t new_size) noexcept override;
    std::error_code sync_data() noexcept override;
    std::error_code sync_all() noexcept override;
    native_handle_type release() override;
    std::error_code assign(native_handle_type handle) noexcept override;

    std::error_code
    open_file(std::filesystem::path const& path, file_base::flags mode);
    void close_file() noexcept;

private:
    /** Pop a recycled op, or allocate on the cold path.

        Per-use fields are filled by the caller; the fields `prepare`-
        style reuse must not inherit from the previous run (`errn`,
        `bytes_transferred`) are reset here. `start()` resets the
        cancellation machinery.
    */
    raf_op* acquire_op()
    {
        raf_op* op;
        {
            std::lock_guard<std::mutex> lock(ops_mutex_);
            op = free_ops_.pop_front();
        }
        if (!op)
            op = new raf_op();
        op->errn              = 0;
        op->bytes_transferred = 0;
        return op;
    }

    posix_random_access_file_service& svc_;
    int fd_ = -1;
    std::mutex ops_mutex_;
    intrusive_list<raf_op> outstanding_ops_;
    intrusive_list<raf_op> free_ops_;
};

// ---------------------------------------------------------------------------
// Inline implementation
// ---------------------------------------------------------------------------

inline posix_random_access_file::posix_random_access_file(
    posix_random_access_file_service& svc) noexcept
    : svc_(svc)
{
}

inline std::error_code
posix_random_access_file::open_file(
    std::filesystem::path const& path, file_base::flags mode)
{
    close_file();

    int oflags = 0;

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
    // Note: no O_APPEND for random access files

    int fd = ::open(path.c_str(), oflags | large_file_open_flag, 0666);
    if (fd < 0)
        return make_err(errno);

    fd_ = fd;

#ifdef POSIX_FADV_RANDOM
    ::posix_fadvise(fd_, 0, 0, POSIX_FADV_RANDOM);
#endif

    return {};
}

inline void
posix_random_access_file::close_file() noexcept
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

inline std::uint64_t
posix_random_access_file::size() const
{
    file_stat_t st;
    if (file_fstat(fd_, &st) < 0)
        throw_system_error(make_err(errno), "random_access_file::size");
    return static_cast<std::uint64_t>(st.st_size);
}

inline std::error_code
posix_random_access_file::resize(std::uint64_t new_size) noexcept
{
    if (new_size >
        static_cast<std::uint64_t>((std::numeric_limits<file_off_t>::max)()))
        return make_err(EOVERFLOW);
    if (file_ftruncate(fd_, static_cast<file_off_t>(new_size)) < 0)
        return make_err(errno);
    return {};
}

inline std::error_code
posix_random_access_file::sync_data() noexcept
{
#if BOOST_COROSIO_HAS_POSIX_SYNCHRONIZED_IO
    if (::fdatasync(fd_) < 0)
#else  // BOOST_COROSIO_HAS_POSIX_SYNCHRONIZED_IO
    if (::fsync(fd_) < 0)
#endif // BOOST_COROSIO_HAS_POSIX_SYNCHRONIZED_IO
        return make_err(errno);
    return {};
}

inline std::error_code
posix_random_access_file::sync_all() noexcept
{
    if (::fsync(fd_) < 0)
        return make_err(errno);
    return {};
}

inline native_handle_type
posix_random_access_file::release()
{
    // A queued op has already copied the fd number; it must not run
    // after the caller closes it and the number is recycled.
    cancel();
    int fd = fd_;
    fd_    = -1;
    return fd;
}

inline std::error_code
posix_random_access_file::assign(native_handle_type handle) noexcept
{
    // The public assign() guarantees the object is closed.
    if (auto ec = validate_file_fd(handle))
        return ec;

    fd_ = handle;
    return {};
}

// read_some_at, write_some_at are defined in
// posix_random_access_file_service.hpp after the service.

// -- raf_op completion handler (scheduler thread) --

inline void
posix_random_access_file::raf_op::operator()()
{
    stop_cb.reset();

    // Empty buffers never reach the pool (diverted at initiation), so
    // empty_buffer stays false and a 0-byte read is a genuine EOF.
    decode_io_result(
        ec_out, bytes_out, cancelled.load(std::memory_order_acquire),
        errn != 0 ? make_err(errn) : std::error_code{}, is_read,
        bytes_transferred, /*empty_buffer=*/false);

    // Copy out everything needed after recycling: once this op is on
    // free_ops_ a concurrent initiation may pop and refill it. The
    // keepalive drops after the push so a final release never runs
    // under ops_mutex_.
    auto keep = std::move(object_ref_);
    auto* c   = awaiting;
    auto exec = ex;
    {
        std::lock_guard<std::mutex> lock(file_->ops_mutex_);
        file_->outstanding_ops_.remove(this);
        file_->free_ops_.push_front(this);
    }
    keep.reset();
    exec.on_work_finished();
    dispatch_coro(exec, *c).resume();
}

// -- raf_op shutdown cleanup --

inline void
posix_random_access_file::raf_op::destroy()
{
    stop_cb.reset();
    auto keep = std::move(object_ref_);
    auto exec = ex;
    {
        std::lock_guard<std::mutex> lock(file_->ops_mutex_);
        file_->outstanding_ops_.remove(this);
        file_->free_ops_.push_front(this);
    }
    keep.reset();
    exec.on_work_finished();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_RANDOM_ACCESS_FILE_HPP
