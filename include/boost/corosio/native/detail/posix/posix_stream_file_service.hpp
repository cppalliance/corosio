//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_STREAM_FILE_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_STREAM_FILE_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/native/detail/posix/posix_stream_file.hpp>
#include <boost/corosio/native/detail/reactor/reactor_scheduler.hpp>
#include <boost/corosio/detail/file_service.hpp>
#include <boost/corosio/detail/object_pool.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/detail/thread_pool.hpp>

#include <vector>

namespace boost::corosio::detail {

/** Stream file service for POSIX backends.

    Owns all posix_stream_file instances. Thread lifecycle is
    managed by the thread_pool service (shared with resolver).
*/
class BOOST_COROSIO_DECL posix_stream_file_service final : public file_service
{
    friend class posix_stream_file;

public:
    explicit posix_stream_file_service(capy::execution_context& ctx)
        : sched_(&get_scheduler(ctx))
        , pool_(ctx)
    {
    }

    ~posix_stream_file_service() override = default;

    posix_stream_file_service(posix_stream_file_service const&) = delete;
    posix_stream_file_service&
    operator=(posix_stream_file_service const&) = delete;

    io_object::implementation* construct() override
    {
        return object_pool_.acquire(*this);
    }

    void destroy(io_object::implementation* p) override
    {
        auto& impl = static_cast<posix_stream_file&>(*p);
        impl.cancel();
        impl.close_file();
        release(&impl);
    }

    void close(io_object::handle& h) override
    {
        if (h.get())
        {
            auto& impl = static_cast<posix_stream_file&>(*h.get());
            impl.cancel();
            impl.close_file();
        }
    }

    std::error_code open_file(
        stream_file::implementation& impl,
        std::filesystem::path const& path,
        file_base::flags mode) override
    {
        // Unavailable in the unsafe tier: the file thread pool completes
        // cross-thread, which the lockless scheduler cannot accept.
        if (sched_->scheduler_locking_disabled())
            return std::make_error_code(std::errc::operation_not_supported);
        return static_cast<posix_stream_file&>(impl).open_file(path, mode);
    }

    void shutdown() override
    {
        // See uring_socket_service_base::shutdown(): snapshot under an
        // acquired reference, cancel+close without the pool lock held;
        // shutdown() sets shutting-down and takes the snapshot in one
        // critical section, so a close that drops the last ref deletes
        // rather than recycles.
        std::vector<posix_stream_file*> live;
        object_pool_.shutdown(
            [&](posix_stream_file* f)
            {
                acquire(f);
                live.push_back(f);
            });
        for (auto* f : live)
        {
            f->cancel();
            f->close_file();
            release(f);
        }
    }

    void post(scheduler_op* op)
    {
        sched_->post(op);
    }

    void work_started() noexcept
    {
        sched_->work_started();
    }

    void work_finished() noexcept
    {
        sched_->work_finished();
    }

    /** Return the thread pool that runs this service's file work.

        The pool's service is created on first use, so this can fail
        where a plain accessor could not. Its workers start later, on
        the first post, and a thread the system refuses there is
        reported by that post rather than thrown here.

        @throws std::bad_alloc If the service cannot be allocated.

        @return The context's shared blocking-I/O pool.

        @see thread_pool_ref::get
    */
    thread_pool& pool()
    {
        return pool_.get();
    }

private:
    scheduler* sched_;
    thread_pool_ref pool_;
    object_pool<posix_stream_file> object_pool_;
};

// ---------------------------------------------------------------------------
// posix_stream_file inline implementations (require complete service type)
// ---------------------------------------------------------------------------

inline std::coroutine_handle<>
posix_stream_file::read_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = read_op_;
    op.reset();
    op.is_read = true;

    // Closed-object contract outranks the zero-length no-op.
    if (fd_ < 0)
    {
        *ec        = make_error_code(std::errc::bad_file_descriptor);
        *bytes_out = 0;
        op.cont.h  = h;
        return dispatch_coro(ex, op.cont);
    }

    capy::mutable_buffer bufs[max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, max_buffers));

    if (op.iovec_count == 0)
    {
        *ec        = {};
        *bytes_out = 0;
        op.cont.h  = h;
        return dispatch_coro(ex, op.cont);
    }

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len  = bufs[i].size();
    }

    op.h         = h;
    op.ex        = ex;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    op.ex.on_work_started();

    read_pool_op_.file_ = this;
    read_pool_op_.ref_  = detail::object_ref(this);
    read_pool_op_.func_ = &posix_stream_file::do_read_work;
    if (auto pec = svc_.pool().post(&read_pool_op_))
    {
        // The pool is shutting down, or the system refused it a thread.
        // Nothing of this read went cross-thread, so it answers here
        // like the closed-descriptor and zero-length exits above rather
        // than through a completion the scheduler has to carry back.
        read_pool_op_.ref_.reset();
        op.stop_cb.reset();
        op.ex.on_work_finished();
        *ec        = pec;
        *bytes_out = 0;
        op.cont.h  = h;
        return dispatch_coro(ex, op.cont);
    }
    return std::noop_coroutine();
}

inline void
posix_stream_file::do_read_work(pool_work_item* w) noexcept
{
    auto* pw   = static_cast<pool_op*>(w);
    auto* self = pw->file_;
    auto& op   = self->read_op_;

    if (!op.cancelled.load(std::memory_order_acquire))
    {
        ssize_t n;
        do
        {
            n = ::preadv(
                self->fd_, op.iovecs, op.iovec_count,
                static_cast<off_t>(self->offset_));
        }
        while (n < 0 && errno == EINTR);

        if (n >= 0)
        {
            op.errn              = 0;
            op.bytes_transferred = static_cast<std::size_t>(n);
            self->offset_ += static_cast<std::uint64_t>(n);
        }
        else
        {
            op.errn              = errno;
            op.bytes_transferred = 0;
        }
    }

    op.object_ref_ = std::move(pw->ref_);
    self->svc_.post(&op);
}

inline std::coroutine_handle<>
posix_stream_file::write_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = write_op_;
    op.reset();
    op.is_read = false;

    // Closed-object contract outranks the zero-length no-op.
    if (fd_ < 0)
    {
        *ec        = make_error_code(std::errc::bad_file_descriptor);
        *bytes_out = 0;
        op.cont.h  = h;
        return dispatch_coro(ex, op.cont);
    }

    capy::mutable_buffer bufs[max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, max_buffers));

    if (op.iovec_count == 0)
    {
        *ec        = {};
        *bytes_out = 0;
        op.cont.h  = h;
        return dispatch_coro(ex, op.cont);
    }

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len  = bufs[i].size();
    }

    op.h         = h;
    op.ex        = ex;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    op.ex.on_work_started();

    write_pool_op_.file_ = this;
    write_pool_op_.ref_  = detail::object_ref(this);
    write_pool_op_.func_ = &posix_stream_file::do_write_work;
    if (auto pec = svc_.pool().post(&write_pool_op_))
    {
        // The pool is shutting down, or the system refused it a thread.
        // Nothing of this write went cross-thread, so it answers here
        // like the closed-descriptor and zero-length exits above rather
        // than through a completion the scheduler has to carry back.
        write_pool_op_.ref_.reset();
        op.stop_cb.reset();
        op.ex.on_work_finished();
        *ec        = pec;
        *bytes_out = 0;
        op.cont.h  = h;
        return dispatch_coro(ex, op.cont);
    }
    return std::noop_coroutine();
}

inline void
posix_stream_file::do_write_work(pool_work_item* w) noexcept
{
    auto* pw   = static_cast<pool_op*>(w);
    auto* self = pw->file_;
    auto& op   = self->write_op_;

    if (!op.cancelled.load(std::memory_order_acquire))
    {
        ssize_t n;
        do
        {
            n = ::pwritev(
                self->fd_, op.iovecs, op.iovec_count,
                static_cast<off_t>(self->offset_));
        }
        while (n < 0 && errno == EINTR);

        if (n >= 0)
        {
            op.errn              = 0;
            op.bytes_transferred = static_cast<std::size_t>(n);
            self->offset_ += static_cast<std::uint64_t>(n);
        }
        else
        {
            op.errn              = errno;
            op.bytes_transferred = 0;
        }
    }

    op.object_ref_ = std::move(pw->ref_);
    self->svc_.post(&op);
}

inline void
posix_stream_file::retire() noexcept
{
    svc_.object_pool_.recycle(this);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_STREAM_FILE_SERVICE_HPP
