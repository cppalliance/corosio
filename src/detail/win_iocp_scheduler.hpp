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
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/core/intrusive_queue.hpp>
#include <boost/system/error_code.hpp>

#include "detail/win_mutex.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

#include "detail/windows.hpp"

namespace boost {
namespace corosio {
namespace detail {

// IOCP completion keys
constexpr std::uintptr_t shutdown_key = 0;
constexpr std::uintptr_t handler_key = 1;
constexpr std::uintptr_t overlapped_key = 2;

using op_queue = capy::intrusive_queue<capy::execution_context::handler>;

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
    void post(capy::any_coro h) const override;
    void post(capy::execution_context::handler* h) const override;
    void on_work_started() noexcept override;
    void on_work_finished() noexcept override;
    bool running_in_this_thread() const noexcept override;
    void stop() override;
    bool stopped() const noexcept override;
    void restart() override;
    std::size_t run() override;
    std::size_t run_one() override;
    std::size_t wait_one(long usec) override;
    std::size_t poll() override;
    std::size_t poll_one() override;

    void* native_handle() const noexcept { return iocp_; }

    // For use by I/O operations to track pending work
    void work_started() const noexcept;
    void work_finished() const noexcept;

private:
    void post_deferred_completions(op_queue& ops);
    std::size_t do_one(unsigned long timeout_ms);

    void* iocp_;
    mutable long outstanding_work_;
    mutable long stopped_;
    long shutdown_;

    // PQCS consumes non-paged pool; limit to one outstanding stop event
    long stop_event_posted_;

    // Signals do_run() to drain completed_ops_ fallback queue
    mutable long dispatch_required_;

    mutable win_mutex dispatch_mutex_;                                      // protects completed_ops_
    mutable op_queue completed_ops_;                                       // fallback when PQCS fails (no auto-destroy)
    std::thread timer_thread_;                                             // placeholder for timer support
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
