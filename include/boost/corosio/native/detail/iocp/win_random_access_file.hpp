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
#include <boost/corosio/native/detail/iocp/win_overlapped_handle.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <coroutine>
#include <cstdint>

namespace boost::corosio::detail {

class win_random_access_file_service;

/** Random-access file state: the concurrent model. */
class win_random_access_file_internal : public win_concurrent_handle
{
    friend class win_random_access_file_service;

public:
    win_random_access_file_internal(
        win_scheduler& sched,
        io_object::implementation& io,
        win_random_access_file_service& svc) noexcept
        : win_concurrent_handle(sched, io)
        , svc_(svc)
    {
    }

    std::uint64_t size() const;
    std::error_code resize(std::uint64_t new_size) noexcept;
    std::error_code sync_data() noexcept;
    std::error_code sync_all() noexcept;

private:
    win_random_access_file_service& svc_;
};

/** Random-access file implementation for IOCP-based I/O.

    Embeds its handle state and delegates every virtual call to it.
    `win_random_access_file_service` recycles instances through its
    pool instead of freeing them on every close.
*/
class win_random_access_file final
    : public random_access_file::implementation
    , public intrusive_list<win_random_access_file>::node
{
    win_random_access_file_service& svc_;
    win_random_access_file_internal internal_;

public:
    explicit win_random_access_file(
        win_random_access_file_service& svc) noexcept;

    /// Recycle into the owning service's pool. Defined out-of-line
    /// after `win_random_access_file_service` is complete.
    void retire() noexcept override;

    /** Assert the closed state a recycled file starts from.

        @pre refs_ == 0, handle closed, no op in flight.
    */
    void reuse() noexcept;

    std::coroutine_handle<> read_some_at(
        std::uint64_t offset,
        capy::continuation& cont,
        capy::executor_ref d,
        buffer_param buf,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override;

    std::coroutine_handle<> write_some_at(
        std::uint64_t offset,
        capy::continuation& cont,
        capy::executor_ref d,
        buffer_param buf,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override;

    native_handle_type native_handle() const noexcept override;
    void cancel() noexcept override;
    std::uint64_t size() const override;
    std::error_code resize(std::uint64_t new_size) noexcept override;
    std::error_code sync_data() noexcept override;
    std::error_code sync_all() noexcept override;
    native_handle_type release() override;
    std::error_code assign(native_handle_type handle) noexcept override;

    win_random_access_file_internal* get_internal() noexcept;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_RANDOM_ACCESS_FILE_HPP
