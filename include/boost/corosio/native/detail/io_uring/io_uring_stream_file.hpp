//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_STREAM_FILE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_STREAM_FILE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IO_URING

#include <boost/corosio/detail/file_service.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_file_ops.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_scheduler.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/stream_file.hpp>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <system_error>
#include <unordered_map>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace boost::corosio::detail {

class io_uring_stream_file_service;

/** Native io_uring stream-file implementation.

    Async `read_some` / `write_some` submit `IORING_OP_READV` /
    `IORING_OP_WRITEV` with `offset == -1` (kernel f_pos). All
    metadata operations (open, size, resize, sync, seek, close)
    are synchronous syscalls.

    @par Thread Safety
    Concurrent `read_some` / `write_some` calls on the same file
    interleave at the kernel level (matches POSIX `read(2)` /
    `write(2)` semantics on a shared positional fd).

    @note On `O_APPEND` open this backend relies on the kernel's
    `f_pos` rather than tracking the offset in user space. Writes
    still go to EOF atomically per `O_APPEND` semantics, but
    `seek(0, seek_cur)` immediately after an append-mode open
    returns `0` (the current f_pos), not the file size — observably
    different from the POSIX backend, which seeds an internal offset
    to size-at-open. Both behaviours are valid; documented for
    cross-backend symmetry.
*/
class BOOST_COROSIO_DECL io_uring_stream_file final
    : public stream_file::implementation
    , public std::enable_shared_from_this<io_uring_stream_file>
    , public intrusive_list<io_uring_stream_file>::node
{
    friend class io_uring_stream_file_service;

    int                  fd_    = -1;
    io_uring_scheduler*  sched_ = nullptr;

    // Per-fd op slots — embedded to eliminate per-call heap allocation.
    // Single-pending invariant per slot.
    uring_file_read_op   rd_;
    uring_file_write_op  wr_;

public:
    explicit io_uring_stream_file(io_uring_scheduler& sched) noexcept
        : sched_(&sched)
    {}

    ~io_uring_stream_file() override
    {
        close_file();
    }

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
        if (fd_ >= 0)
            sched_->submit_cancel_by_fd(fd_);
    }

    std::uint64_t size() const override
    {
        struct stat st;
        if (::fstat(fd_, &st) < 0)
            throw_system_error(make_err(errno), "stream_file::size");
        return static_cast<std::uint64_t>(st.st_size);
    }

    void resize(std::uint64_t new_size) override
    {
        if (new_size > static_cast<std::uint64_t>(
                (std::numeric_limits<off_t>::max)()))
            throw_system_error(
                make_err(EOVERFLOW), "stream_file::resize");
        if (::ftruncate(fd_, static_cast<off_t>(new_size)) < 0)
            throw_system_error(make_err(errno), "stream_file::resize");
    }

    void sync_data() override
    {
#if BOOST_COROSIO_HAS_POSIX_SYNCHRONIZED_IO
        if (::fdatasync(fd_) < 0)
#else
        if (::fsync(fd_) < 0)
#endif
            throw_system_error(
                make_err(errno), "stream_file::sync_data");
    }

    void sync_all() override
    {
        if (::fsync(fd_) < 0)
            throw_system_error(make_err(errno), "stream_file::sync_all");
    }

    native_handle_type release() override
    {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void assign(native_handle_type handle) override
    {
        close_file();
        fd_ = handle;
    }

    std::uint64_t seek(
        std::int64_t offset, file_base::seek_basis origin) override
    {
        int whence = SEEK_SET;
        if (origin == file_base::seek_cur) whence = SEEK_CUR;
        else if (origin == file_base::seek_end) whence = SEEK_END;

        off_t r = ::lseek(fd_, static_cast<off_t>(offset), whence);
        if (r == static_cast<off_t>(-1))
            throw_system_error(make_err(errno), "stream_file::seek");
        return static_cast<std::uint64_t>(r);
    }

    // -- Internal --

    /// Open the file. Synchronous; sets `fd_`. Caller is the service.
    std::error_code open_file(
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
        if ((mode & file_base::append) != file_base::flags(0))
            oflags |= O_APPEND;
        if ((mode & file_base::sync_all_on_write) != file_base::flags(0))
            oflags |= O_SYNC;

        oflags |= O_CLOEXEC;

        int fd = ::open(path.c_str(), oflags, 0666);
        if (fd < 0)
            return make_err(errno);

        fd_ = fd;

#ifdef POSIX_FADV_SEQUENTIAL
        // Hint the page cache about the access pattern; matches the
        // POSIX backend.
        ::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

        return {};
    }

    /// Cancel any in-flight ops and close the fd. Idempotent.
    void close_file() noexcept
    {
        if (fd_ >= 0)
        {
            sched_->cancel_and_flush(fd_);
            ::close(fd_);
            fd_ = -1;
        }
    }
};

inline std::coroutine_handle<>
io_uring_stream_file::read_some(
    std::coroutine_handle<> h,
    capy::executor_ref      ex,
    buffer_param            buffers,
    std::stop_token         token,
    std::error_code*        ec,
    std::size_t*            bytes)
{
    rd_.prepare(h, ex, ec, bytes, fd_, /*file_offset=*/-1, sched_,
        shared_from_this(), buffers, token);
    sched_->work_started();

    if (rd_.empty_buffer ||
        rd_.cancelled.load(std::memory_order_acquire))
    {
        io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
        sched_->push_completed_locked(&rd_);
        return std::noop_coroutine();
    }

    io_uring_submit_op(*sched_, &rd_);
    return std::noop_coroutine();
}

inline std::coroutine_handle<>
io_uring_stream_file::write_some(
    std::coroutine_handle<> h,
    capy::executor_ref      ex,
    buffer_param            buffers,
    std::stop_token         token,
    std::error_code*        ec,
    std::size_t*            bytes)
{
    wr_.prepare(h, ex, ec, bytes, fd_, /*file_offset=*/-1, sched_,
        shared_from_this(), buffers, token);
    sched_->work_started();

    if (wr_.empty_buffer ||
        wr_.cancelled.load(std::memory_order_acquire))
    {
        io_uring_scheduler::lock_type lock(sched_->dispatch_mutex());
        sched_->push_completed_locked(&wr_);
        return std::noop_coroutine();
    }

    io_uring_submit_op(*sched_, &wr_);
    return std::noop_coroutine();
}

/** Native io_uring stream-file service.

    Owns all `io_uring_stream_file` impls. Replaces
    `posix_stream_file_service` for the io_uring backend; registered
    under the abstract `file_service` key by `io_uring_t::construct`.
*/
class BOOST_COROSIO_DECL io_uring_stream_file_service final
    : public file_service
{
public:
    explicit io_uring_stream_file_service(
        capy::execution_context& /*ctx*/, io_uring_scheduler& sched)
        : sched_(&sched)
    {}

    ~io_uring_stream_file_service() override = default;

    io_uring_stream_file_service(
        io_uring_stream_file_service const&)            = delete;
    io_uring_stream_file_service& operator=(
        io_uring_stream_file_service const&)            = delete;

    io_object::implementation* construct() override
    {
        auto ptr   = std::make_shared<io_uring_stream_file>(*sched_);
        auto* impl = ptr.get();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            file_list_.push_back(impl);
            file_ptrs_[impl] = std::move(ptr);
        }
        return impl;
    }

    void destroy(io_object::implementation* p) override
    {
        // close_file() already does cancel_and_flush(fd_) before
        // ::close — calling cancel() too would queue a redundant
        // cancel-by-fd SQE that finds nothing.
        auto& impl = static_cast<io_uring_stream_file&>(*p);
        impl.close_file();
        destroy_impl(impl);
    }

    void close(io_object::handle& h) override
    {
        if (h.get())
            static_cast<io_uring_stream_file&>(*h.get()).close_file();
    }

    std::error_code open_file(
        stream_file::implementation& impl,
        std::filesystem::path const& path,
        file_base::flags mode) override
    {
        return static_cast<io_uring_stream_file&>(impl).open_file(
            path, mode);
    }

    void shutdown() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* impl = file_list_.pop_front(); impl != nullptr;
             impl       = file_list_.pop_front())
        {
            impl->close_file();
        }
        file_ptrs_.clear();
    }

private:
    void destroy_impl(io_uring_stream_file& impl)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        file_list_.remove(&impl);
        file_ptrs_.erase(&impl);
    }

    io_uring_scheduler*                       sched_;
    std::mutex                                mutex_;
    intrusive_list<io_uring_stream_file>      file_list_;
    std::unordered_map<
        io_uring_stream_file*,
        std::shared_ptr<io_uring_stream_file>> file_ptrs_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IO_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_STREAM_FILE_HPP
