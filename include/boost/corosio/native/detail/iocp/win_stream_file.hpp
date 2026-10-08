//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_STREAM_FILE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_STREAM_FILE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/stream_file.hpp>
#include <boost/corosio/file_base.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <coroutine>
#include <cstdint>

namespace boost::corosio::detail {

class win_file_service;
class win_stream_file;

/** Read operation state for stream file IOCP I/O. */
struct file_read_op : overlapped_op
{
    void* buf     = nullptr;
    DWORD buf_len = 0;
    win_stream_file& file_;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit file_read_op(win_stream_file& f) noexcept;
};

/** Write operation state for stream file IOCP I/O. */
struct file_write_op : overlapped_op
{
    void* buf     = nullptr;
    DWORD buf_len = 0;
    win_stream_file& file_;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit file_write_op(win_stream_file& f) noexcept;
};

/** Stream file implementation for IOCP-based I/O.

    Collapses the historical internal-state/wrapper split into one
    pooled `io_object::implementation`: it both owns the native
    handle and pending operations, and directly implements
    `stream_file::implementation`. `win_file_service` recycles
    instances through its pool instead of freeing them on every
    close.
*/
class win_stream_file final
    : public stream_file::implementation
    , public intrusive_list<win_stream_file>::node
{
    friend class win_file_service;
    friend struct file_read_op;
    friend struct file_write_op;

    win_file_service& svc_;
    file_read_op rd_;
    file_write_op wr_;
    HANDLE handle_        = INVALID_HANDLE_VALUE;
    std::uint64_t offset_ = 0;

public:
    explicit win_stream_file(win_file_service& svc) noexcept;

    /// Recycle into the owning service's pool. Defined out-of-line
    /// after `win_file_service` is complete (needs `svc_.pool_`).
    void retire() noexcept override;

    /** Reset recycled state for reuse.

        Each op's `OVERLAPPED` fields are re-seeded by
        `overlapped_op::reset()` at its next submit, so they are not
        zeroed here.

        @pre refs_ == 0, handle closed, no op in flight.
    */
    void reuse() noexcept;

    std::coroutine_handle<> read_some(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        buffer_param buf,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override;

    std::coroutine_handle<> write_some(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        buffer_param buf,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override;

    native_handle_type native_handle() const noexcept override;
    bool is_open() const noexcept;
    void cancel() noexcept override;
    std::uint64_t size() const override;
    std::error_code resize(std::uint64_t new_size) noexcept override;
    std::error_code sync_data() noexcept override;
    std::error_code sync_all() noexcept override;
    native_handle_type release() override;
    std::error_code assign(native_handle_type handle) noexcept override;
    capy::io_result<std::uint64_t>
    seek(std::int64_t offset, file_base::seek_basis origin) noexcept override;

    void close_handle() noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_STREAM_FILE_HPP
