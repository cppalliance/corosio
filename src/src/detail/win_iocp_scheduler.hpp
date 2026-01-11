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

#include <boost/corosio/detail/scheduler.hpp>
#include <boost/capy/execution_context.hpp>

#include <atomic>
#include <chrono>

namespace boost {
namespace corosio {
namespace detail {

class win_iocp_scheduler
    : public scheduler
    , public capy::execution_context::service
{
public:
    using key_type = scheduler;

     win_iocp_scheduler(
        capy::execution_context& ctx,
        unsigned concurrency_hint = 0);

    ~win_iocp_scheduler();

    win_iocp_scheduler(win_iocp_scheduler const&) = delete;
    win_iocp_scheduler& operator=(win_iocp_scheduler const&) = delete;

    void shutdown() override;
    void post(capy::coro h) const override;
    void post(capy::executor_work* w) const override;

    void defer(capy::coro h) const override
    {
        post(h);
    }

    void on_work_started() noexcept override
    {
        outstanding_work_.fetch_add(1, std::memory_order_relaxed);
    }

    void on_work_finished() noexcept override
    {
        outstanding_work_.fetch_sub(1, std::memory_order_relaxed);
    }

    bool running_in_this_thread() const noexcept override;
    void stop() override;
    bool stopped() const noexcept override;
    void restart() override;
    std::size_t run(system::error_code& ec) override;
    std::size_t run_one(system::error_code& ec) override;
    std::size_t run_one(long usec, system::error_code& ec) override;
    std::size_t wait_one(long usec, system::error_code& ec) override;
    std::size_t run_for(std::chrono::steady_clock::duration rel_time) override;
    std::size_t run_until(std::chrono::steady_clock::time_point abs_time) override;
    std::size_t poll(system::error_code& ec) override;
    std::size_t poll_one(system::error_code& ec) override;

    void* native_handle() const noexcept { return iocp_; }

    void work_started() const noexcept
    {
        pending_.fetch_add(1, std::memory_order_relaxed);
    }

    void work_finished() const noexcept
    {
        pending_.fetch_sub(1, std::memory_order_relaxed);
    }

private:
    std::size_t do_run(unsigned long timeout, std::size_t max_handlers,
        system::error_code& ec);
    std::size_t do_wait(unsigned long timeout, system::error_code& ec);

    void* iocp_;
    mutable std::atomic<std::size_t> pending_{0};
    mutable std::atomic<std::size_t> outstanding_work_{0};
    std::atomic<bool> stopped_{false};
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // _WIN32

#endif
