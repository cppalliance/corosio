//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_REACTIVE_SCHEDULER_HPP
#define BOOST_COROSIO_DETAIL_REACTIVE_SCHEDULER_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/capy/execution_context.hpp>
#include <boost/capy/executor.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace boost {
namespace corosio {
namespace detail {

class reactor;

template<bool isUnsafe = false>
class reactive_scheduler
    : public scheduler
    , public capy::execution_context::service
{
public:
    using key_type = scheduler;

    explicit reactive_scheduler(
        capy::execution_context& ctx,
        unsigned concurrency_hint);

    ~reactive_scheduler();

    reactive_scheduler(reactive_scheduler const&) = delete;
    reactive_scheduler& operator=(reactive_scheduler const&) = delete;

    void shutdown() override;
    void init_task();
    void post(capy::coro h) const override;
    void post(capy::executor_work* w) const override;
    void defer(capy::coro h) const override;
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

private:
    struct thread_info;

    template<bool U>
    friend struct work_cleanup;

    template<bool U>
    friend struct task_cleanup;

    std::size_t do_run(
        std::unique_lock<std::mutex>& lock,
        thread_info& this_thread,
        std::size_t max_handlers);

    bool one_thread_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    mutable capy::executor_work_queue queue_;
    std::size_t outstanding_work_ = 0;
    bool stopped_ = false;
    bool shutdown_ = false;
    reactor* task_ = nullptr;
    struct task_op : capy::executor_work
    {
        void operator()() override {}
        void destroy() override {}
    } task_operation_;
    bool task_interrupted_ = true;
};

extern template class reactive_scheduler<false>;
extern template class reactive_scheduler<true>;

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
