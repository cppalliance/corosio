//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_WIN_IOCP_SCHEDULER_HPP
#define BOOST_COROSIO_DETAIL_WIN_IOCP_SCHEDULER_HPP

#include <boost/corosio/detail/config.hpp>

#ifdef _WIN32

#if defined(_WIN32_WINNT) && (_WIN32_WINNT < 0x0600)
#error "corosio requires Windows Vista or later (_WIN32_WINNT >= 0x0600)"
#endif

#include <boost/corosio/detail/scheduler.hpp>
#include <boost/capy/execution_context.hpp>
#include <boost/capy/intrusive_list.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

namespace boost {
namespace corosio {
namespace detail {

// IOCP completion keys
constexpr std::uintptr_t shutdown_key = 0;
constexpr std::uintptr_t handler_key = 1;
constexpr std::uintptr_t socket_key = 2;

// Forward declaration
struct overlapped_op;

class win_iocp_scheduler
    : public scheduler
    , public capy::execution_context::service
{
public:
    using key_type = scheduler;

    win_iocp_scheduler(
        capy::execution_context& ctx,
        int concurrency_hint = -1);
    ~win_iocp_scheduler();
    win_iocp_scheduler(win_iocp_scheduler const&) = delete;
    win_iocp_scheduler& operator=(win_iocp_scheduler const&) = delete;

    void shutdown() override;
    void post(capy::coro h) const override;
    void post(capy::execution_context::handler* h) const override;
    void on_work_started() noexcept override;
    void on_work_finished() noexcept override;
    bool running_in_this_thread() const noexcept override;
    void stop() override;
    bool stopped() const noexcept override;
    void restart() override;
    std::size_t run() override;
    std::size_t run_one() override;
    std::size_t run_one(long usec) override;
    std::size_t wait_one(long usec) override;
    std::size_t run_for(std::chrono::steady_clock::duration rel_time) override;
    std::size_t run_until(std::chrono::steady_clock::time_point abs_time) override;
    std::size_t poll() override;
    std::size_t poll_one() override;

    void* native_handle() const noexcept { return iocp_; }

    // For use by I/O operations to track pending work
    void work_started() const noexcept;
    void work_finished() const noexcept;

private:
    std::size_t do_run(unsigned long timeout, std::size_t max_handlers,
        system::error_code& ec);
    std::size_t do_wait(unsigned long timeout, system::error_code& ec);

    void* iocp_;
    mutable long outstanding_work_;
    mutable long stopped_;
    long shutdown_;

    // PQCS consumes non-paged pool; limit to one outstanding stop event
    long stop_event_posted_;

    // Signals do_run() to drain completed_ops_ fallback queue
    mutable long dispatch_required_;

    mutable std::mutex dispatch_mutex_;                                    // protects completed_ops_
    mutable capy::intrusive_list<capy::execution_context::handler> completed_ops_; // fallback when PQCS fails (no auto-destroy)
    std::thread timer_thread_;                                             // placeholder for timer support
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // _WIN32

#endif
