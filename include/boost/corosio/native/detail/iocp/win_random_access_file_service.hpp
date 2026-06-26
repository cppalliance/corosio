//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_RANDOM_ACCESS_FILE_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_RANDOM_ACCESS_FILE_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/random_access_file_service.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_mutex.hpp>
#include <boost/corosio/native/detail/iocp/win_random_access_file.hpp>
#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/native/detail/iocp/win_completion_key.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/detail/buffer_param.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/error.hpp>

#include <filesystem>

namespace boost::corosio::detail {

/** Windows IOCP random-access file management service.

    Owns all random-access file implementations and coordinates
    their lifecycle with the IOCP.

    @par Thread Safety
    All public member functions are thread-safe.
*/
class BOOST_COROSIO_DECL win_random_access_file_service final
    : public random_access_file_service
{
public:
    using key_type = win_random_access_file_service;

    explicit win_random_access_file_service(capy::execution_context& ctx);
    ~win_random_access_file_service();

    win_random_access_file_service(
        win_random_access_file_service const&)            = delete;
    win_random_access_file_service& operator=(
        win_random_access_file_service const&) = delete;

    io_object::implementation* construct() override;
    void destroy(io_object::implementation* p) override;
    void close(io_object::handle& h) override;
    void shutdown() override;

    std::error_code open_file(
        random_access_file::implementation& impl,
        std::filesystem::path const& path,
        file_base::flags mode) override;

    void destroy_impl(win_random_access_file& impl);
    void unregister_impl(win_random_access_file_internal& impl);

    void post(overlapped_op* op);
    void on_pending(overlapped_op* op) noexcept;
    void on_completion(overlapped_op* op, DWORD error, DWORD bytes) noexcept;
    void work_started() noexcept;
    void work_finished() noexcept;

    void* iocp_handle() const noexcept;

    /** Attempt data-only flush via NtFlushBuffersFileEx.

        @return true if data-only flush succeeded, false if
        caller should fall back to FlushFileBuffers.
    */
    bool try_flush_data(HANDLE h) noexcept;

private:
    // NtFlushBuffersFileEx support for data-only sync
    struct io_status_block
    {
        union { LONG Status; void* Pointer; };
        ULONG_PTR Information;
    };

    enum { flush_flags_file_data_sync_only = 4 };

    using nt_flush_fn = LONG(NTAPI*)(
        HANDLE, ULONG, void*, ULONG, io_status_block*);

    win_scheduler& sched_;
    BOOST_COROSIO_MSVC_WARNING_PUSH
    BOOST_COROSIO_MSVC_WARNING_DISABLE(4251) // detail:: members, dll-interface
    win_mutex mutex_;
    intrusive_list<win_random_access_file_internal> file_list_;
    intrusive_list<win_random_access_file> wrapper_list_;
    BOOST_COROSIO_MSVC_WARNING_POP
    void* iocp_;
    nt_flush_fn nt_flush_buffers_file_ex_;
};

/** Get or create the random-access file service for the given context. */
inline win_random_access_file_service&
get_random_access_file_service(capy::execution_context& ctx, win_scheduler&)
{
    return ctx.make_service<win_random_access_file_service>();
}

// ---------------------------------------------------------------------------
// raf_concurrent_op
// ---------------------------------------------------------------------------

inline raf_concurrent_op::raf_concurrent_op(
    win_random_access_file_internal& f) noexcept
    : overlapped_op(&do_complete)
    , file_(&f)
{
    cancel_func_ = &do_cancel_impl;
}

inline void
raf_concurrent_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<raf_concurrent_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->file_->is_open())
        ::CancelIoEx(op->file_->native_handle(), op);
}

inline void
raf_concurrent_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<raf_concurrent_op*>(base);

    if (!owner)
    {
        // Shutdown path: clean up without invoking
        op->stop_cb.reset();
        op->h = {};
        {
            std::lock_guard<win_mutex> lock(op->file_->ops_mutex_);
            op->file_->outstanding_ops_.remove(op);
        }
        op->file_ref.reset();
        delete op;
        return;
    }

    // Normal completion
    op->stop_cb.reset();

    if (op->ec_out)
    {
        if (op->cancelled.load(std::memory_order_acquire))
            *op->ec_out = capy::error::canceled;
        else if (op->dwError != 0)
            *op->ec_out = make_err(op->dwError);
        else if (op->is_read && op->bytes_transferred == 0 && !op->empty_buffer)
            *op->ec_out = capy::error::eof;
        else
            *op->ec_out = {};
    }

    if (op->bytes_out)
        *op->bytes_out = static_cast<std::size_t>(op->bytes_transferred);

    {
        std::lock_guard<win_mutex> lock(op->file_->ops_mutex_);
        op->file_->outstanding_ops_.remove(op);
    }

    op->file_ref.reset();

    auto coro = op->h;
    delete op;
    coro.resume();
}

// ---------------------------------------------------------------------------
// win_random_access_file_internal
// ---------------------------------------------------------------------------

inline
win_random_access_file_internal::win_random_access_file_internal(
    win_random_access_file_service& svc) noexcept
    : svc_(svc)
{
}

inline
win_random_access_file_internal::~win_random_access_file_internal()
{
    svc_.unregister_impl(*this);
}

inline HANDLE
win_random_access_file_internal::native_handle() const noexcept
{
    return handle_;
}

inline bool
win_random_access_file_internal::is_open() const noexcept
{
    return handle_ != INVALID_HANDLE_VALUE;
}

inline void
win_random_access_file_internal::cancel() noexcept
{
    if (handle_ != INVALID_HANDLE_VALUE)
        ::CancelIoEx(handle_, nullptr);

    std::lock_guard<win_mutex> lock(ops_mutex_);
    outstanding_ops_.for_each([](raf_concurrent_op* op) {
        op->request_cancel();
    });
}

inline void
win_random_access_file_internal::close_handle() noexcept
{
    if (handle_ != INVALID_HANDLE_VALUE)
    {
        ::CancelIoEx(handle_, nullptr);
        ::CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
}

inline std::uint64_t
win_random_access_file_internal::size() const
{
    LARGE_INTEGER li;
    if (!::GetFileSizeEx(handle_, &li))
        throw_system_error(
            make_err(::GetLastError()), "random_access_file::size");
    return static_cast<std::uint64_t>(li.QuadPart);
}

inline void
win_random_access_file_internal::resize(std::uint64_t new_size)
{
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(new_size);
    if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN))
        throw_system_error(
            make_err(::GetLastError()), "random_access_file::resize");
    if (!::SetEndOfFile(handle_))
        throw_system_error(
            make_err(::GetLastError()), "random_access_file::resize");
}

inline void
win_random_access_file_internal::sync_data()
{
    // Attempt data-only flush; fall back to full flush
    if (svc_.try_flush_data(handle_))
        return;
    if (!::FlushFileBuffers(handle_))
        throw_system_error(
            make_err(::GetLastError()), "random_access_file::sync_data");
}

inline void
win_random_access_file_internal::sync_all()
{
    if (!::FlushFileBuffers(handle_))
        throw_system_error(
            make_err(::GetLastError()), "random_access_file::sync_all");
}

inline native_handle_type
win_random_access_file_internal::release()
{
    HANDLE h = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    return reinterpret_cast<native_handle_type>(h);
}

inline void
win_random_access_file_internal::assign(native_handle_type handle)
{
    close_handle();
    HANDLE h = reinterpret_cast<HANDLE>(handle);
    // Register with IOCP so overlapped I/O works
    if (!::CreateIoCompletionPort(
            h, static_cast<HANDLE>(svc_.iocp_handle()), key_io, 0))
    {
        throw_system_error(
            make_err(::GetLastError()), "random_access_file::assign");
    }
    handle_ = h;
}

inline std::coroutine_handle<>
win_random_access_file_internal::read_some_at(
    std::uint64_t offset,
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    static constexpr std::size_t max_buffers = 16;

    auto* op = new raf_concurrent_op(*this);
    op->file_ref = shared_from_this();

    op->reset();
    op->is_read  = true;
    op->h         = h;
    op->ex        = ex;
    op->ec_out    = ec;
    op->bytes_out = bytes_out;
    op->start(token);

    svc_.work_started();

    capy::mutable_buffer bufs[max_buffers];
    auto count = param.copy_to(bufs, max_buffers);

    if (count == 0)
    {
        op->empty_buffer = true;
        {
            std::lock_guard<win_mutex> lock(ops_mutex_);
            outstanding_ops_.push_back(op);
        }
        svc_.on_completion(op, 0, 0);
        return std::noop_coroutine();
    }

    op->buf     = bufs[0].data();
    op->buf_len = static_cast<DWORD>(bufs[0].size());

    // Set caller-provided offset in OVERLAPPED
    op->Offset     = static_cast<DWORD>(offset & 0xFFFFFFFF);
    op->OffsetHigh = static_cast<DWORD>(offset >> 32);

    {
        std::lock_guard<win_mutex> lock(ops_mutex_);
        outstanding_ops_.push_back(op);
    }

    BOOL ok = ::ReadFile(handle_, op->buf, op->buf_len, nullptr, op);
    DWORD err = ok ? 0 : ::GetLastError();

    if (err != 0 && err != ERROR_IO_PENDING)
    {
        svc_.on_completion(op, err, 0);
        return std::noop_coroutine();
    }

    svc_.on_pending(op);

    // Re-check cancellation after I/O is pending
    if (op->cancelled.load(std::memory_order_acquire))
        ::CancelIoEx(handle_, op);

    return std::noop_coroutine();
}

inline std::coroutine_handle<>
win_random_access_file_internal::write_some_at(
    std::uint64_t offset,
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    static constexpr std::size_t max_buffers = 16;

    auto* op = new raf_concurrent_op(*this);
    op->file_ref = shared_from_this();

    op->reset();
    op->is_read  = false;
    op->h         = h;
    op->ex        = ex;
    op->ec_out    = ec;
    op->bytes_out = bytes_out;
    op->start(token);

    svc_.work_started();

    capy::mutable_buffer bufs[max_buffers];
    auto count = param.copy_to(bufs, max_buffers);

    if (count == 0)
    {
        {
            std::lock_guard<win_mutex> lock(ops_mutex_);
            outstanding_ops_.push_back(op);
        }
        svc_.on_completion(op, 0, 0);
        return std::noop_coroutine();
    }

    op->buf     = bufs[0].data();
    op->buf_len = static_cast<DWORD>(bufs[0].size());

    // Set caller-provided offset in OVERLAPPED
    op->Offset     = static_cast<DWORD>(offset & 0xFFFFFFFF);
    op->OffsetHigh = static_cast<DWORD>(offset >> 32);

    {
        std::lock_guard<win_mutex> lock(ops_mutex_);
        outstanding_ops_.push_back(op);
    }

    BOOL ok = ::WriteFile(handle_, op->buf, op->buf_len, nullptr, op);
    DWORD err = ok ? 0 : ::GetLastError();

    if (err != 0 && err != ERROR_IO_PENDING)
    {
        svc_.on_completion(op, err, 0);
        return std::noop_coroutine();
    }

    svc_.on_pending(op);

    // Re-check cancellation after I/O is pending
    if (op->cancelled.load(std::memory_order_acquire))
        ::CancelIoEx(handle_, op);

    return std::noop_coroutine();
}

// ---------------------------------------------------------------------------
// win_random_access_file wrapper
// ---------------------------------------------------------------------------

inline
win_random_access_file::win_random_access_file(
    std::shared_ptr<win_random_access_file_internal> internal) noexcept
    : internal_(std::move(internal))
{
}

inline void
win_random_access_file::close_internal() noexcept
{
    if (internal_)
    {
        internal_->close_handle();
        internal_.reset();
    }
}

inline std::coroutine_handle<>
win_random_access_file::read_some_at(
    std::uint64_t offset,
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param buf,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes)
{
    return internal_->read_some_at(offset, h, d, buf, token, ec, bytes);
}

inline std::coroutine_handle<>
win_random_access_file::write_some_at(
    std::uint64_t offset,
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param buf,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes)
{
    return internal_->write_some_at(offset, h, d, buf, token, ec, bytes);
}

inline native_handle_type
win_random_access_file::native_handle() const noexcept
{
    return reinterpret_cast<native_handle_type>(internal_->native_handle());
}

inline void
win_random_access_file::cancel() noexcept
{
    internal_->cancel();
}

inline std::uint64_t
win_random_access_file::size() const
{
    return internal_->size();
}

inline void
win_random_access_file::resize(std::uint64_t new_size)
{
    internal_->resize(new_size);
}

inline void
win_random_access_file::sync_data()
{
    internal_->sync_data();
}

inline void
win_random_access_file::sync_all()
{
    internal_->sync_all();
}

inline native_handle_type
win_random_access_file::release()
{
    return internal_->release();
}

inline void
win_random_access_file::assign(native_handle_type handle)
{
    internal_->assign(handle);
}

inline win_random_access_file_internal*
win_random_access_file::get_internal() const noexcept
{
    return internal_.get();
}

// ---------------------------------------------------------------------------
// win_random_access_file_service
// ---------------------------------------------------------------------------

inline
win_random_access_file_service::win_random_access_file_service(
    capy::execution_context& ctx)
    : sched_(ctx.use_service<win_scheduler>())
    , iocp_(sched_.native_handle())
    , nt_flush_buffers_file_ex_(nullptr)
{
    if (FARPROC p = ::GetProcAddress(
            ::GetModuleHandleA("NTDLL"), "NtFlushBuffersFileEx"))
    {
        nt_flush_buffers_file_ex_ = reinterpret_cast<nt_flush_fn>(
            reinterpret_cast<void*>(p));
    }
}

inline
win_random_access_file_service::~win_random_access_file_service()
{
    for (auto* w = wrapper_list_.pop_front(); w != nullptr;
         w       = wrapper_list_.pop_front())
        delete w;
}

inline io_object::implementation*
win_random_access_file_service::construct()
{
    auto internal =
        std::make_shared<win_random_access_file_internal>(*this);

    {
        std::lock_guard<win_mutex> lock(mutex_);
        file_list_.push_back(internal.get());
    }

    auto* wrapper = new win_random_access_file(std::move(internal));

    {
        std::lock_guard<win_mutex> lock(mutex_);
        wrapper_list_.push_back(wrapper);
    }

    return wrapper;
}

inline void
win_random_access_file_service::destroy(io_object::implementation* p)
{
    if (p)
    {
        auto& wrapper = static_cast<win_random_access_file&>(*p);
        wrapper.close_internal();
        destroy_impl(wrapper);
    }
}

inline void
win_random_access_file_service::close(io_object::handle& h)
{
    auto& wrapper = static_cast<win_random_access_file&>(*h.get());
    wrapper.get_internal()->close_handle();
}

inline void
win_random_access_file_service::shutdown()
{
    std::lock_guard<win_mutex> lock(mutex_);

    for (auto* impl = file_list_.pop_front(); impl != nullptr;
         impl       = file_list_.pop_front())
    {
        impl->close_handle();
    }
}

inline std::error_code
win_random_access_file_service::open_file(
    random_access_file::implementation& impl,
    std::filesystem::path const& path,
    file_base::flags mode)
{
    // Build access mask
    DWORD access = 0;
    unsigned a   = static_cast<unsigned>(mode) & 3u;
    if (a == 3)
        access = GENERIC_READ | GENERIC_WRITE;
    else if (a == 2)
        access = GENERIC_WRITE;
    else
        access = GENERIC_READ;

    // Build creation disposition
    DWORD disposition = OPEN_EXISTING;
    if ((mode & file_base::create) && (mode & file_base::exclusive))
        disposition = CREATE_NEW;
    else if ((mode & file_base::create) && (mode & file_base::truncate))
        disposition = OPEN_ALWAYS;
    else if (mode & file_base::create)
        disposition = OPEN_ALWAYS;
    else if (mode & file_base::truncate)
        disposition = TRUNCATE_EXISTING;

    // Build flags — FILE_FLAG_OVERLAPPED + FILE_FLAG_RANDOM_ACCESS
    DWORD flags = FILE_ATTRIBUTE_NORMAL
                | FILE_FLAG_OVERLAPPED
                | FILE_FLAG_RANDOM_ACCESS;
    if (mode & file_base::sync_all_on_write)
        flags |= FILE_FLAG_WRITE_THROUGH;

    HANDLE h = ::CreateFileW(
        path.c_str(),
        access,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        disposition,
        flags,
        nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return make_err(::GetLastError());

    // Register with IOCP
    if (!::CreateIoCompletionPort(
            h, static_cast<HANDLE>(iocp_), key_io, 0))
    {
        DWORD err = ::GetLastError();
        ::CloseHandle(h);
        return make_err(err);
    }

    // Handle truncation for create|truncate combo
    if ((mode & file_base::create) && (mode & file_base::truncate)
        && disposition == OPEN_ALWAYS)
    {
        if (!::SetEndOfFile(h))
        {
            DWORD err = ::GetLastError();
            ::CloseHandle(h);
            return make_err(err);
        }
    }

    auto& internal =
        *static_cast<win_random_access_file&>(impl).get_internal();
    internal.handle_ = h;

    return {};
}

inline void
win_random_access_file_service::destroy_impl(win_random_access_file& impl)
{
    {
        std::lock_guard<win_mutex> lock(mutex_);
        wrapper_list_.remove(&impl);
    }
    delete &impl;
}

inline void
win_random_access_file_service::unregister_impl(
    win_random_access_file_internal& impl)
{
    std::lock_guard<win_mutex> lock(mutex_);
    file_list_.remove(&impl);
}

inline void
win_random_access_file_service::post(overlapped_op* op)
{
    sched_.post(op);
}

inline void
win_random_access_file_service::on_pending(overlapped_op* op) noexcept
{
    sched_.on_pending(op);
}

inline void
win_random_access_file_service::on_completion(
    overlapped_op* op, DWORD error, DWORD bytes) noexcept
{
    sched_.on_completion(op, error, bytes);
}

inline void
win_random_access_file_service::work_started() noexcept
{
    sched_.work_started();
}

inline void
win_random_access_file_service::work_finished() noexcept
{
    sched_.work_finished();
}

inline void*
win_random_access_file_service::iocp_handle() const noexcept
{
    return iocp_;
}

inline bool
win_random_access_file_service::try_flush_data(HANDLE h) noexcept
{
    if (nt_flush_buffers_file_ex_)
    {
        io_status_block status = {};
        if (nt_flush_buffers_file_ex_(
                h, flush_flags_file_data_sync_only,
                nullptr, 0, &status) == 0)
            return true;
    }
    return false;
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_RANDOM_ACCESS_FILE_SERVICE_HPP
