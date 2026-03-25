//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_RANDOM_ACCESS_FILE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_RANDOM_ACCESS_FILE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/random_access_file.hpp>
#include <boost/corosio/file_base.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_mutex.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <coroutine>
#include <cstdint>
#include <memory>

namespace boost::corosio::detail {

class win_random_access_file_service;
class win_random_access_file_internal;

/** Per-operation state for concurrent random-access file IOCP I/O.

    Heap-allocated for each async read/write, enabling unlimited
    concurrent operations on the same file. Self-deletes on
    completion or shutdown.
*/
struct raf_concurrent_op
    : overlapped_op
    , intrusive_list<raf_concurrent_op>::node
{
    void* buf      = nullptr;
    DWORD buf_len  = 0;
    win_random_access_file_internal* file_ = nullptr;
    std::shared_ptr<win_random_access_file_internal> file_ref;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit raf_concurrent_op(
        win_random_access_file_internal& f) noexcept;
};

/** Internal random-access file state for IOCP-based I/O.

    Each async operation heap-allocates a raf_concurrent_op,
    allowing unlimited concurrent reads and writes.
*/
class win_random_access_file_internal
    : public intrusive_list<win_random_access_file_internal>::node
    , public std::enable_shared_from_this<win_random_access_file_internal>
{
    friend class win_random_access_file_service;
    friend class win_random_access_file;
    friend struct raf_concurrent_op;

    win_random_access_file_service& svc_;
    win_mutex ops_mutex_;
    intrusive_list<raf_concurrent_op> outstanding_ops_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;

public:
    explicit win_random_access_file_internal(
        win_random_access_file_service& svc) noexcept;
    ~win_random_access_file_internal();

    std::coroutine_handle<> read_some_at(
        std::uint64_t offset,
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*);

    std::coroutine_handle<> write_some_at(
        std::uint64_t offset,
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
};

/** Random-access file implementation wrapper for IOCP-based I/O. */
class win_random_access_file final
    : public random_access_file::implementation
    , public intrusive_list<win_random_access_file>::node
{
    std::shared_ptr<win_random_access_file_internal> internal_;

public:
    explicit win_random_access_file(
        std::shared_ptr<win_random_access_file_internal> internal) noexcept;

    void close_internal() noexcept;

    std::coroutine_handle<> read_some_at(
        std::uint64_t offset,
        std::coroutine_handle<> h,
        capy::executor_ref d,
        buffer_param buf,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override;

    std::coroutine_handle<> write_some_at(
        std::uint64_t offset,
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

    win_random_access_file_internal* get_internal() const noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_RANDOM_ACCESS_FILE_HPP
