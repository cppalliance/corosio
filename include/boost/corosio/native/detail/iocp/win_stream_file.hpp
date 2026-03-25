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
#include <boost/corosio/stream_file.hpp>
#include <boost/corosio/file_base.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <coroutine>
#include <cstdint>
#include <memory>

namespace boost::corosio::detail {

class win_file_service;
class win_stream_file_internal;

/** Read operation state for stream file IOCP I/O. */
struct file_read_op : overlapped_op
{
    void* buf      = nullptr;
    DWORD buf_len  = 0;
    win_stream_file_internal& file_;
    std::shared_ptr<win_stream_file_internal> file_ptr;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit file_read_op(win_stream_file_internal& f) noexcept;
};

/** Write operation state for stream file IOCP I/O. */
struct file_write_op : overlapped_op
{
    void* buf      = nullptr;
    DWORD buf_len  = 0;
    win_stream_file_internal& file_;
    std::shared_ptr<win_stream_file_internal> file_ptr;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit file_write_op(win_stream_file_internal& f) noexcept;
};

/** Internal stream file state for IOCP-based I/O.

    Contains the actual state for a single file, including
    the native handle, position tracking, and pending operations.
    Derives from enable_shared_from_this so operations can extend
    its lifetime.
*/
class win_stream_file_internal
    : public intrusive_list<win_stream_file_internal>::node
    , public std::enable_shared_from_this<win_stream_file_internal>
{
    friend class win_file_service;
    friend class win_stream_file;
    friend struct file_read_op;
    friend struct file_write_op;

    win_file_service& svc_;
    file_read_op rd_;
    file_write_op wr_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::uint64_t offset_ = 0;

public:
    explicit win_stream_file_internal(win_file_service& svc) noexcept;
    ~win_stream_file_internal();

    std::coroutine_handle<> read_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*);

    std::coroutine_handle<> write_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*);

    HANDLE native_handle() const noexcept;
    bool is_open() const noexcept;
    void cancel() noexcept;
    void close_handle() noexcept;

    std::uint64_t size() const;
    void resize(std::uint64_t new_size);
    void sync_data();
    void sync_all();
    native_handle_type release();
    void assign(native_handle_type handle);
    std::uint64_t seek(std::int64_t offset, file_base::seek_basis origin);
};

/** Stream file implementation wrapper for IOCP-based I/O.

    Public-facing implementation that holds a shared_ptr to
    the internal state. Delegates all virtual calls.
*/
class win_stream_file final
    : public stream_file::implementation
    , public intrusive_list<win_stream_file>::node
{
    std::shared_ptr<win_stream_file_internal> internal_;

public:
    explicit win_stream_file(
        std::shared_ptr<win_stream_file_internal> internal) noexcept;

    void close_internal() noexcept;

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
    void cancel() noexcept override;
    std::uint64_t size() const override;
    void resize(std::uint64_t new_size) override;
    void sync_data() override;
    void sync_all() override;
    native_handle_type release() override;
    void assign(native_handle_type handle) override;
    std::uint64_t seek(std::int64_t offset, file_base::seek_basis origin) override;

    win_stream_file_internal* get_internal() const noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_STREAM_FILE_HPP
