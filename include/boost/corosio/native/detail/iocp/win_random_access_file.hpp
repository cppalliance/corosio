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
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/random_access_file.hpp>
#include <boost/corosio/file_base.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_mutex.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <coroutine>
#include <mutex>
#include <cstdint>

namespace boost::corosio::detail {

class win_random_access_file_service;
class win_random_access_file;

/** Per-operation state for concurrent random-access file IOCP I/O.

    Acquired from the file's free list for each async read/write,
    enabling unlimited concurrent operations on the same file.
    Recycles into that free list on completion or shutdown, so
    steady-state reads and writes allocate nothing; the file
    destructor frees the recycled storage.
*/
struct raf_concurrent_op
    : overlapped_op
    , intrusive_list<raf_concurrent_op>::node
{
    void* buf                      = nullptr;
    DWORD buf_len                  = 0;
    win_random_access_file* file_ = nullptr;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);
    static void do_cancel_impl(overlapped_op* op) noexcept;

    explicit raf_concurrent_op(win_random_access_file& f) noexcept;
};

/** Random-access file implementation for IOCP-based I/O.

    Collapses the historical internal-state/wrapper split into one
    pooled `io_object::implementation`. Each async operation acquires
    a `raf_concurrent_op` from the per-file free list, allowing
    unlimited concurrent reads and writes without steady-state
    allocation.
*/
class win_random_access_file final
    : public random_access_file::implementation
    , public intrusive_list<win_random_access_file>::node
{
    friend class win_random_access_file_service;
    friend struct raf_concurrent_op;

    win_random_access_file_service& svc_;
    win_mutex ops_mutex_;
    intrusive_list<raf_concurrent_op> outstanding_ops_;
    /// Recycled ops (guarded by `ops_mutex_`). Survives impl
    /// recycling — the storage belongs to this impl and only the
    /// destructor frees it.
    intrusive_list<raf_concurrent_op> free_ops_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;

    /// Pop a recycled op (re-arming its keepalive), or allocate on
    /// the cold path; the caller's `reset()` clears per-use state.
    raf_concurrent_op* acquire_op()
    {
        raf_concurrent_op* op;
        {
            std::lock_guard<win_mutex> lock(ops_mutex_);
            op = free_ops_.pop_front();
        }
        if (!op)
            op = new raf_concurrent_op(*this);
        op->object_ref_ = detail::object_ref(this);
        return op;
    }

public:
    explicit win_random_access_file(
        win_random_access_file_service& svc) noexcept;

    /// Recycle into the owning service's pool. Defined out-of-line
    /// after `win_random_access_file_service` is complete (needs
    /// `svc_.pool_`).
    void retire() noexcept override;

    /** Reset recycled state for reuse.

        `free_ops_` deliberately survives recycling; only the
        destructor frees it.

        @pre refs_ == 0, handle closed, no op in flight (per-op
        `raf_concurrent_op` holds its own `object_ref`, so `refs_`
        cannot reach zero while one is outstanding).
    */
    void reuse() noexcept;

    ~win_random_access_file() override
    {
        while (auto* op = free_ops_.pop_front())
            delete op;
    }

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
    bool is_open() const noexcept;
    void cancel() noexcept override;
    std::uint64_t size() const override;
    std::error_code resize(std::uint64_t new_size) noexcept override;
    std::error_code sync_data() noexcept override;
    std::error_code sync_all() noexcept override;
    native_handle_type release() override;
    std::error_code assign(native_handle_type handle) noexcept override;

    void close_handle() noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_RANDOM_ACCESS_FILE_HPP
