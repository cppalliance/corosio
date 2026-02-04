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

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <system_error>

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
    static void on_timer_changed(void* ctx);
    void post_deferred_completions(op_queue& ops);
    std::size_t do_one(unsigned long timeout_ms);

    void* iocp_;
    mutable long outstanding_work_;
    mutable long stopped_;
    long shutdown_;
    long stop_event_posted_;
    mutable long dispatch_required_;

    mutable win_mutex dispatch_mutex_;
    mutable op_queue completed_ops_;
    std::unique_ptr<win_timers> timers_;
    timer_service* timer_svc_ = nullptr;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_DETAIL_IOCP_SCHEDULER_HPP
