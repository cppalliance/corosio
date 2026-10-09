//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_FILE_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_FILE_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/file_service.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/detail/object_pool.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/native/detail/iocp/win_mutex.hpp>
#include <boost/corosio/native/detail/iocp/win_stream_file.hpp>
#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/native/detail/iocp/win_completion_key.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/detail/buffer_param.hpp>
#include <boost/capy/buffers.hpp>

#include <filesystem>

namespace boost::corosio::detail {

/** Windows IOCP stream file management service.

    Owns all stream file implementations and coordinates their
    lifecycle with the IOCP.

    @par Thread Safety
    All public member functions are thread-safe.
*/
class BOOST_COROSIO_DECL win_file_service final : public file_service
{
    friend class win_stream_file;

public:
    using key_type = win_file_service;

    explicit win_file_service(capy::execution_context& ctx);
    ~win_file_service();

    win_file_service(win_file_service const&)            = delete;
    win_file_service& operator=(win_file_service const&) = delete;

    io_object::implementation* construct() override;
    void destroy(io_object::implementation* p) override;
    void close(io_object::handle& h) override;
    void shutdown() override;

    std::error_code open_file(
        stream_file::implementation& impl,
        std::filesystem::path const& path,
        file_base::flags mode) override;

    /** Attempt data-only flush via NtFlushBuffersFileEx.

        @return true if data-only flush succeeded, false if
        caller should fall back to FlushFileBuffers.
    */
    bool try_flush_data(HANDLE h) noexcept;

private:
    // NtFlushBuffersFileEx support for data-only sync
    struct io_status_block
    {
        union
        {
            LONG Status;
            void* Pointer;
        };
        ULONG_PTR Information;
    };

    enum
    {
        flush_flags_file_data_sync_only = 4
    };

    using nt_flush_fn =
        LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, io_status_block*);

    win_scheduler& sched_;
    BOOST_COROSIO_MSVC_WARNING_PUSH
    BOOST_COROSIO_MSVC_WARNING_DISABLE(4251) // detail:: members, dll-interface
    object_pool<win_stream_file> pool_;
    BOOST_COROSIO_MSVC_WARNING_POP
    void* iocp_;
    nt_flush_fn nt_flush_buffers_file_ex_;
};

/** Get or create the stream file service for the given context. */
inline win_file_service&
get_stream_file_service(capy::execution_context& ctx, win_scheduler&)
{
    return ctx.make_service<win_file_service>();
}

// ---------------------------------------------------------------------------
// win_stream_file_internal
// ---------------------------------------------------------------------------

inline std::uint64_t
win_stream_file_internal::size() const
{
    LARGE_INTEGER li;
    if (!::GetFileSizeEx(handle_, &li))
        throw_system_error(make_err(::GetLastError()), "stream_file::size");
    return static_cast<std::uint64_t>(li.QuadPart);
}

inline std::error_code
win_stream_file_internal::resize(std::uint64_t new_size) noexcept
{
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(new_size);
    if (!::SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN))
        return make_err(::GetLastError());
    if (!::SetEndOfFile(handle_))
        return make_err(::GetLastError());
    return {};
}

inline std::error_code
win_stream_file_internal::sync_data() noexcept
{
    // Attempt data-only flush; fall back to full flush
    if (svc_.try_flush_data(handle_))
        return {};
    if (!::FlushFileBuffers(handle_))
        return make_err(::GetLastError());
    return {};
}

inline std::error_code
win_stream_file_internal::sync_all() noexcept
{
    if (!::FlushFileBuffers(handle_))
        return make_err(::GetLastError());
    return {};
}

inline capy::io_result<std::uint64_t>
win_stream_file_internal::seek(
    std::int64_t offset, file_base::seek_basis origin) noexcept
{
    // We manage offset_ ourselves (same as POSIX impl).
    std::int64_t new_pos;

    if (origin == file_base::seek_set)
    {
        new_pos = offset;
    }
    else if (origin == file_base::seek_cur)
    {
        new_pos = static_cast<std::int64_t>(
                      offset_.load(std::memory_order_acquire)) +
            offset;
    }
    else // seek_end
    {
        LARGE_INTEGER li;
        if (!::GetFileSizeEx(handle_, &li))
            return {make_err(::GetLastError()), 0};
        new_pos = li.QuadPart + offset;
    }

    if (new_pos < 0)
        return {make_err(ERROR_NEGATIVE_SEEK), 0};

    offset_.store(
        static_cast<std::uint64_t>(new_pos), std::memory_order_release);
    return {std::error_code{}, static_cast<std::uint64_t>(new_pos)};
}

// ---------------------------------------------------------------------------
// win_stream_file wrapper
// ---------------------------------------------------------------------------

inline win_stream_file::win_stream_file(win_file_service& svc) noexcept
    : svc_(svc)
    , internal_(svc.sched_, *this, svc)
{
}

inline void
win_stream_file::retire() noexcept
{
    svc_.pool_.recycle(this);
}

inline void
win_stream_file::reuse() noexcept
{
    BOOST_COROSIO_ASSERT(!internal_.is_open());
}

inline std::coroutine_handle<>
win_stream_file::read_some(
    capy::continuation& cont,
    capy::executor_ref d,
    buffer_param buf,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes)
{
    return internal_.read_some(cont, d, buf, token, ec, bytes);
}

inline std::coroutine_handle<>
win_stream_file::write_some(
    capy::continuation& cont,
    capy::executor_ref d,
    buffer_param buf,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes)
{
    return internal_.write_some(cont, d, buf, token, ec, bytes);
}

inline native_handle_type
win_stream_file::native_handle() const noexcept
{
    return reinterpret_cast<native_handle_type>(internal_.native_handle());
}

inline void
win_stream_file::cancel() noexcept
{
    internal_.cancel();
}

inline std::uint64_t
win_stream_file::size() const
{
    return internal_.size();
}

inline std::error_code
win_stream_file::resize(std::uint64_t new_size) noexcept
{
    return internal_.resize(new_size);
}

inline std::error_code
win_stream_file::sync_data() noexcept
{
    return internal_.sync_data();
}

inline std::error_code
win_stream_file::sync_all() noexcept
{
    return internal_.sync_all();
}

inline native_handle_type
win_stream_file::release()
{
    return internal_.release();
}

inline std::error_code
win_stream_file::assign(native_handle_type handle) noexcept
{
    return internal_.assign(handle, handle_kind::stream_file);
}

inline capy::io_result<std::uint64_t>
win_stream_file::seek(
    std::int64_t offset, file_base::seek_basis origin) noexcept
{
    return internal_.seek(offset, origin);
}

inline win_stream_file_internal*
win_stream_file::get_internal() noexcept
{
    return &internal_;
}

// ---------------------------------------------------------------------------
// win_file_service
// ---------------------------------------------------------------------------

inline win_file_service::win_file_service(capy::execution_context& ctx)
    : sched_(ctx.use_service<win_scheduler>())
    , iocp_(sched_.native_handle())
    , nt_flush_buffers_file_ex_(nullptr)
{
    if (FARPROC p = ::GetProcAddress(
            ::GetModuleHandleA("NTDLL"), "NtFlushBuffersFileEx"))
    {
        nt_flush_buffers_file_ex_ =
            reinterpret_cast<nt_flush_fn>(reinterpret_cast<void*>(p));
    }
}

inline win_file_service::~win_file_service() = default;

inline io_object::implementation*
win_file_service::construct()
{
    return pool_.acquire(*this);
}

inline void
win_file_service::destroy(io_object::implementation* p)
{
    if (!p)
        return;
    auto* f = static_cast<win_stream_file*>(p);
    f->get_internal()->close_handle();
    release(f);
}

inline void
win_file_service::close(io_object::handle& h)
{
    static_cast<win_stream_file&>(*h.get()).get_internal()->close_handle();
}

inline void
win_file_service::shutdown()
{
    pool_.shutdown(
        [](win_stream_file* f) { f->get_internal()->close_handle(); });
}

inline std::error_code
win_file_service::open_file(
    stream_file::implementation& impl,
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

    // Build flags — FILE_FLAG_OVERLAPPED is required for IOCP
    DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED |
        FILE_FLAG_SEQUENTIAL_SCAN;
    if (mode & file_base::sync_all_on_write)
        flags |= FILE_FLAG_WRITE_THROUGH;

    HANDLE h = ::CreateFileW(
        path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        disposition, flags, nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return make_err(::GetLastError());

    // Register with IOCP
    if (!::CreateIoCompletionPort(h, static_cast<HANDLE>(iocp_), key_io, 0))
    {
        DWORD err = ::GetLastError();
        ::CloseHandle(h);
        return make_err(err);
    }

    // Handle truncation for create|truncate combo
    if ((mode & file_base::create) && (mode & file_base::truncate) &&
        disposition == OPEN_ALWAYS)
    {
        if (!::SetEndOfFile(h))
        {
            DWORD err = ::GetLastError();
            ::CloseHandle(h);
            return make_err(err);
        }
    }

    auto& internal   = *static_cast<win_stream_file&>(impl).get_internal();
    internal.handle_ = h;
    internal.offset_.store(0, std::memory_order_release);

    // Handle append: seek to end
    if (mode & file_base::append)
    {
        LARGE_INTEGER sz;
        if (!::GetFileSizeEx(h, &sz))
        {
            DWORD err        = ::GetLastError();
            internal.handle_ = INVALID_HANDLE_VALUE;
            ::CloseHandle(h);
            return make_err(err);
        }
        internal.offset_.store(
            static_cast<std::uint64_t>(sz.QuadPart), std::memory_order_release);
    }

    return {};
}

inline bool
win_file_service::try_flush_data(HANDLE h) noexcept
{
    if (nt_flush_buffers_file_ex_)
    {
        io_status_block status = {};
        if (nt_flush_buffers_file_ex_(
                h, flush_flags_file_data_sync_only, nullptr, 0, &status) == 0)
            return true;
    }
    return false;
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_FILE_SERVICE_HPP
