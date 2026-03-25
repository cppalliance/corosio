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

    Each async read/write heap-allocates an raf_op that serves
    as both the thread-pool work item and the scheduler completion
    op. This allows unlimited concurrent operations on the same
    file object, matching Asio's per-op allocation model.

    The raf_op self-deletes on completion or shutdown.
*/

namespace boost::corosio::detail {

struct scheduler;
class posix_random_access_file_service;

/** Random-access file implementation for POSIX backends. */
class posix_random_access_file final
    : public random_access_file::implementation
    , public std::enable_shared_from_this<posix_random_access_file>
    , public intrusive_list<posix_random_access_file>::node
{
    friend class posix_random_access_file_service;

public:
    static constexpr std::size_t max_buffers = 16;

    /** Per-operation state, heap-allocated for each async call.

        Inherits from scheduler_op (for scheduler completion) and
        pool_work_item (for thread-pool dispatch). Linked into the
        file's outstanding_ops_ list for cancellation tracking.
    */
    struct raf_op final
        : scheduler_op
        , pool_work_item
        , intrusive_list<raf_op>::node
    {
        struct canceller
        {
            raf_op* op;
            void operator()() const noexcept
            {
                op->cancelled.store(true, std::memory_order_release);
            }
        };

        std::coroutine_handle<> h;
        capy::executor_ref ex;

        std::error_code* ec_out = nullptr;
        std::size_t* bytes_out  = nullptr;

        iovec iovecs[max_buffers];
        int iovec_count = 0;
        std::uint64_t offset = 0;

        int errn                    = 0;
        std::size_t bytes_transferred = 0;
        bool is_read                = false;

        std::atomic<bool> cancelled{false};
        std::optional<std::stop_callback<canceller>> stop_cb;

        posix_random_access_file* file_ = nullptr;
        std::shared_ptr<posix_random_access_file> file_ref;

        void start(std::stop_token const& token)
        {
            cancelled.store(false, std::memory_order_release);
            stop_cb.reset();
            if (token.stop_possible())
                stop_cb.emplace(token, canceller{this});
        }

        void operator()() override;
        void destroy() override;

        /// Thread-pool work function: executes preadv/pwritev.
        static void do_work(pool_work_item*) noexcept;
    };

    explicit posix_random_access_file(
        posix_random_access_file_service& svc) noexcept;

    // -- random_access_file::implementation --

    std::coroutine_handle<> read_some_at(
        std::uint64_t offset,
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*) override;

    std::coroutine_handle<> write_some_at(
        std::uint64_t offset,
        std::coroutine_handle<>,
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
    void resize(std::uint64_t new_size) override;
    void sync_data() override;
    void sync_all() override;
    native_handle_type release() override;
    void assign(native_handle_type handle) override;

    std::error_code open_file(
        std::filesystem::path const& path, file_base::flags mode);
    void close_file() noexcept;

private:
    posix_random_access_file_service& svc_;
    int fd_ = -1;
    std::mutex ops_mutex_;
    intrusive_list<raf_op> outstanding_ops_;
};

// ---------------------------------------------------------------------------
// Inline implementation
// ---------------------------------------------------------------------------

inline
posix_random_access_file::posix_random_access_file(
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

    int fd = ::open(path.c_str(), oflags, 0666);
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
    struct stat st;
    if (::fstat(fd_, &st) < 0)
        throw_system_error(make_err(errno), "random_access_file::size");
    return static_cast<std::uint64_t>(st.st_size);
}

inline void
posix_random_access_file::resize(std::uint64_t new_size)
{
    if (new_size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        throw_system_error(make_err(EOVERFLOW), "random_access_file::resize");
    if (::ftruncate(fd_, static_cast<off_t>(new_size)) < 0)
        throw_system_error(make_err(errno), "random_access_file::resize");
}

inline void
posix_random_access_file::sync_data()
{
#if BOOST_COROSIO_HAS_POSIX_SYNCHRONIZED_IO
    if (::fdatasync(fd_) < 0)
#else // BOOST_COROSIO_HAS_POSIX_SYNCHRONIZED_IO
    if (::fsync(fd_) < 0)
#endif // BOOST_COROSIO_HAS_POSIX_SYNCHRONIZED_IO
        throw_system_error(make_err(errno), "random_access_file::sync_data");
}

inline void
posix_random_access_file::sync_all()
{
    if (::fsync(fd_) < 0)
        throw_system_error(make_err(errno), "random_access_file::sync_all");
}

inline native_handle_type
posix_random_access_file::release()
{
    int fd = fd_;
    fd_ = -1;
    return fd;
}

inline void
posix_random_access_file::assign(native_handle_type handle)
{
    close_file();
    fd_ = handle;
}

// read_some_at, write_some_at are defined in
// posix_random_access_file_service.hpp after the service.

// -- raf_op completion handler (scheduler thread) --

inline void
posix_random_access_file::raf_op::operator()()
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

    {
        std::lock_guard<std::mutex> lock(file_->ops_mutex_);
        file_->outstanding_ops_.remove(this);
    }

    file_ref.reset();

    auto coro = h;
    ex.on_work_finished();
    delete this;
    coro.resume();
}

// -- raf_op shutdown cleanup --

inline void
posix_random_access_file::raf_op::destroy()
{
    stop_cb.reset();
    {
        std::lock_guard<std::mutex> lock(file_->ops_mutex_);
        file_->outstanding_ops_.remove(this);
    }
    file_ref.reset();
    ex.on_work_finished();
    delete this;
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_RANDOM_ACCESS_FILE_HPP
