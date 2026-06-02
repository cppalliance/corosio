//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_RANDOM_ACCESS_FILE_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_RANDOM_ACCESS_FILE_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/native/detail/posix/posix_random_access_file.hpp>
#include <boost/corosio/native/detail/reactor/reactor_scheduler.hpp>
#include <boost/corosio/detail/random_access_file_service.hpp>
#include <boost/corosio/detail/thread_pool.hpp>

#include <limits>
#include <mutex>
#include <unordered_map>

namespace boost::corosio::detail {

/** Random-access file service for POSIX backends. */
class BOOST_COROSIO_DECL posix_random_access_file_service final
    : public random_access_file_service
{
public:
    posix_random_access_file_service(
        capy::execution_context& ctx, scheduler& sched)
        : sched_(&sched)
        , pool_(get_or_create_pool(ctx))
    {
    }

    ~posix_random_access_file_service() override = default;

    posix_random_access_file_service(
        posix_random_access_file_service const&)            = delete;
    posix_random_access_file_service& operator=(
        posix_random_access_file_service const&) = delete;

    io_object::implementation* construct() override
    {
        auto ptr   = std::make_shared<posix_random_access_file>(*this);
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
        auto& impl = static_cast<posix_random_access_file&>(*p);
        impl.cancel();
        impl.close_file();
        destroy_impl(impl);
    }

    void close(io_object::handle& h) override
    {
        if (h.get())
        {
            auto& impl = static_cast<posix_random_access_file&>(*h.get());
            impl.cancel();
            impl.close_file();
        }
    }

    std::error_code open_file(
        random_access_file::implementation& impl,
        std::filesystem::path const& path,
        file_base::flags mode) override
    {
        if (sched_->is_single_threaded())
            return std::make_error_code(std::errc::operation_not_supported);
        return static_cast<posix_random_access_file&>(impl).open_file(
            path, mode);
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

    void destroy_impl(posix_random_access_file& impl)
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
    intrusive_list<posix_random_access_file> file_list_;
    std::unordered_map<
        posix_random_access_file*,
        std::shared_ptr<posix_random_access_file>>
        file_ptrs_;
};

/** Get or create the random-access file service for the given context. */
inline posix_random_access_file_service&
get_random_access_file_service(capy::execution_context& ctx, scheduler& sched)
{
    return ctx.make_service<posix_random_access_file_service>(sched);
}

// ---------------------------------------------------------------------------
// posix_random_access_file inline implementations (require complete service)
// ---------------------------------------------------------------------------

inline std::coroutine_handle<>
posix_random_access_file::read_some_at(
    std::uint64_t offset,
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    capy::mutable_buffer bufs[max_buffers];
    auto count = param.copy_to(bufs, max_buffers);

    if (count == 0)
    {
        *ec        = {};
        *bytes_out = 0;
        return h;
    }

    auto* op = new raf_op();
    op->is_read = true;
    op->offset  = offset;

    op->iovec_count = static_cast<int>(count);
    for (int i = 0; i < op->iovec_count; ++i)
    {
        op->iovecs[i].iov_base = bufs[i].data();
        op->iovecs[i].iov_len  = bufs[i].size();
    }

    op->h         = h;
    op->ex        = ex;
    op->ec_out    = ec;
    op->bytes_out = bytes_out;
    op->file_     = this;
    op->file_ref  = this->shared_from_this();
    op->start(token);

    op->ex.on_work_started();

    {
        std::lock_guard<std::mutex> lock(ops_mutex_);
        outstanding_ops_.push_back(op);
    }

    static_cast<pool_work_item*>(op)->func_ = &raf_op::do_work;
    if (!svc_.pool().post(static_cast<pool_work_item*>(op)))
    {
        op->cancelled.store(true, std::memory_order_release);
        svc_.post(static_cast<scheduler_op*>(op));
    }
    return std::noop_coroutine();
}

inline std::coroutine_handle<>
posix_random_access_file::write_some_at(
    std::uint64_t offset,
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    capy::mutable_buffer bufs[max_buffers];
    auto count = param.copy_to(bufs, max_buffers);

    if (count == 0)
    {
        *ec        = {};
        *bytes_out = 0;
        return h;
    }

    auto* op = new raf_op();
    op->is_read = false;
    op->offset  = offset;

    op->iovec_count = static_cast<int>(count);
    for (int i = 0; i < op->iovec_count; ++i)
    {
        op->iovecs[i].iov_base = bufs[i].data();
        op->iovecs[i].iov_len  = bufs[i].size();
    }

    op->h         = h;
    op->ex        = ex;
    op->ec_out    = ec;
    op->bytes_out = bytes_out;
    op->file_     = this;
    op->file_ref  = this->shared_from_this();
    op->start(token);

    op->ex.on_work_started();

    {
        std::lock_guard<std::mutex> lock(ops_mutex_);
        outstanding_ops_.push_back(op);
    }

    static_cast<pool_work_item*>(op)->func_ = &raf_op::do_work;
    if (!svc_.pool().post(static_cast<pool_work_item*>(op)))
    {
        op->cancelled.store(true, std::memory_order_release);
        svc_.post(static_cast<scheduler_op*>(op));
    }
    return std::noop_coroutine();
}

// -- raf_op thread-pool work function --

inline void
posix_random_access_file::raf_op::do_work(pool_work_item* w) noexcept
{
    auto* op   = static_cast<raf_op*>(w);
    auto* self = op->file_;

    if (op->cancelled.load(std::memory_order_acquire))
    {
        op->errn              = ECANCELED;
        op->bytes_transferred = 0;
    }
    else if (op->offset >
             static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
    {
        op->errn              = EOVERFLOW;
        op->bytes_transferred = 0;
    }
    else
    {
        ssize_t n;
        if (op->is_read)
        {
            do
            {
                n = ::preadv(self->fd_, op->iovecs, op->iovec_count,
                             static_cast<off_t>(op->offset));
            }
            while (n < 0 && errno == EINTR);
        }
        else
        {
            do
            {
                n = ::pwritev(self->fd_, op->iovecs, op->iovec_count,
                              static_cast<off_t>(op->offset));
            }
            while (n < 0 && errno == EINTR);
        }

        if (n >= 0)
        {
            op->errn              = 0;
            op->bytes_transferred = static_cast<std::size_t>(n);
        }
        else
        {
            op->errn              = errno;
            op->bytes_transferred = 0;
        }
    }

    self->svc_.post(static_cast<scheduler_op*>(op));
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_RANDOM_ACCESS_FILE_SERVICE_HPP
