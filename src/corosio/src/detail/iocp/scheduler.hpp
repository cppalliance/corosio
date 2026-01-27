//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_IOCP_SCHEDULER_HPP
#define BOOST_COROSIO_DETAIL_IOCP_SCHEDULER_HPP


#if defined(_WIN32)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/system/error_code.hpp>

#include "src/detail/scheduler_op.hpp"
#include "src/detail/iocp/completion_key.hpp"
#include "src/detail/iocp/mutex.hpp"

#include <chrono>
#include <cstdint>
#include <memory>

#include "src/detail/iocp/windows.hpp"

namespace boost::corosio::detail {

// Forward declarations
struct overlapped_op;
class win_timers;
class timer_service;

class win_scheduler
    : public scheduler
    , public capy::execution_context::service
{
public:
    using key_type = scheduler;

    win_scheduler(
        capy::execution_context& ctx,
        int concurrency_hint = -1);
    ~win_scheduler();
    win_scheduler(win_scheduler const&) = delete;
    win_scheduler& operator=(win_scheduler const&) = delete;

    void shutdown() override;
    void post(capy::coro h) const override;
    void post(scheduler_op* h) const override;
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
    void work_started() const noexcept override;
    void work_finished() const noexcept override;

    // Timer service integration
    void set_timer_service(timer_service* svc);
    void update_timeout();

private:
    // Completion key for posted handlers (scheduler_op*)
    struct handler_key final : completion_key
    {
        result on_completion(
            win_scheduler& sched,
            DWORD bytes,
            DWORD dwError,
            LPOVERLAPPED overlapped) override;

        void destroy(LPOVERLAPPED overlapped) override;
    };

    // Completion key for stop signaling
    struct shutdown_key final : completion_key
    {
        result on_completion(
            win_scheduler& sched,
            DWORD bytes,
            DWORD dwError,
            LPOVERLAPPED overlapped) override;
    };

    // Static callback thunk - receives 'this' as context
    static void on_timer_changed(void* ctx);
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

    handler_key handler_key_;
    shutdown_key shutdown_key_;

    mutable win_mutex dispatch_mutex_;                                      // protects completed_ops_
    mutable op_queue completed_ops_;                                       // fallback when PQCS fails (no auto-destroy)
    std::unique_ptr<win_timers> timers_;                                   // timer wakeup mechanism
    timer_service* timer_svc_ = nullptr;                                   // timer service for processing
};

} // namespace boost::corosio::detail

#endif // _WIN32

#endif // BOOST_COROSIO_DETAIL_IOCP_SCHEDULER_HPP
