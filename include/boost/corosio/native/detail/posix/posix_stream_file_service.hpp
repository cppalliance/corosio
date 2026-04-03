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
#include <boost/corosio/detail/thread_pool.hpp>

#include <mutex>
#include <unordered_map>

namespace boost::corosio::detail {

/** Stream file service for POSIX backends.

    Owns all posix_stream_file instances. Thread lifecycle is
    managed by the thread_pool service (shared with resolver).
*/
class BOOST_COROSIO_DECL posix_stream_file_service final
    : public file_service
{
public:
    posix_stream_file_service(
        capy::execution_context& ctx, scheduler& sched)
        : sched_(&sched)
        , pool_(get_or_create_pool(ctx))
    {
    }

    ~posix_stream_file_service() override = default;

    posix_stream_file_service(posix_stream_file_service const&)            = delete;
    posix_stream_file_service& operator=(posix_stream_file_service const&) = delete;

    io_object::implementation* construct() override
    {
        auto ptr   = std::make_shared<posix_stream_file>(*this);
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
        auto& impl = static_cast<posix_stream_file&>(*p);
        impl.cancel();
        impl.close_file();
        destroy_impl(impl);
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
        if (static_cast<reactor_scheduler const*>(sched_)->is_single_threaded())
            return std::make_error_code(std::errc::operation_not_supported);
        return static_cast<posix_stream_file&>(impl).open_file(path, mode);
    }

    void shutdown() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* impl = file_list_.pop_front(); impl != nullptr;
             impl       = file_list_.pop_front())
        {
            impl->cancel();
            impl->close_file();
        }
        file_ptrs_.clear();
    }

    void destroy_impl(posix_stream_file& impl)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        file_list_.remove(&impl);
        file_ptrs_.erase(&impl);
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

    thread_pool& pool() noexcept
    {
        return pool_;
    }

private:
    static thread_pool& get_or_create_pool(capy::execution_context& ctx)
    {
        auto* p = ctx.find_service<thread_pool>();
        if (p)
            return *p;
        return ctx.make_service<thread_pool>();
    }

    scheduler* sched_;
    thread_pool& pool_;
    std::mutex mutex_;
    intrusive_list<posix_stream_file> file_list_;
    std::unordered_map<posix_stream_file*, std::shared_ptr<posix_stream_file>>
        file_ptrs_;
};

/** Get or create the stream file service for the given context. */
inline posix_stream_file_service&
get_stream_file_service(capy::execution_context& ctx, scheduler& sched)
{
    return ctx.make_service<posix_stream_file_service>(sched);
}

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

    capy::mutable_buffer bufs[max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, max_buffers));

    if (op.iovec_count == 0)
    {
        *ec        = {};
        *bytes_out = 0;
        op.cont_op.cont.h = h;
        return dispatch_coro(ex, op.cont_op.cont);
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
    read_pool_op_.ref_  = this->shared_from_this();
    read_pool_op_.func_ = &posix_stream_file::do_read_work;
    if (!svc_.pool().post(&read_pool_op_))
    {
        op.impl_ref = std::move(read_pool_op_.ref_);
        op.cancelled.store(true, std::memory_order_release);
        svc_.post(&read_op_);
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
            n = ::preadv(self->fd_, op.iovecs, op.iovec_count,
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

    op.impl_ref = std::move(pw->ref_);
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

    capy::mutable_buffer bufs[max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, max_buffers));

    if (op.iovec_count == 0)
    {
        *ec        = {};
        *bytes_out = 0;
        op.cont_op.cont.h = h;
        return dispatch_coro(ex, op.cont_op.cont);
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
    write_pool_op_.ref_  = this->shared_from_this();
    write_pool_op_.func_ = &posix_stream_file::do_write_work;
    if (!svc_.pool().post(&write_pool_op_))
    {
        op.impl_ref = std::move(write_pool_op_.ref_);
        op.cancelled.store(true, std::memory_order_release);
        svc_.post(&write_op_);
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
            n = ::pwritev(self->fd_, op.iovecs, op.iovec_count,
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

    op.impl_ref = std::move(pw->ref_);
    self->svc_.post(&op);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_STREAM_FILE_SERVICE_HPP
