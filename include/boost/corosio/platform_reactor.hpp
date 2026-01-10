//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_PLATFORM_REACTOR_HPP
#define BOOST_COROSIO_PLATFORM_REACTOR_HPP

#include <boost/capy/service_provider.hpp>
#include <boost/capy/executor_work.hpp>

#include <mutex>

namespace boost {
namespace corosio {

/** Abstract base class for platform reactor services.

    A platform reactor simulates an operating system level I/O event loop
    such as epoll (Linux), io_uring (Linux), or I/O Completion Ports
    (Windows). Derived classes provide thread-safe or single-threaded
    implementations.

    @see platform_reactor_single
    @see platform_reactor_multi
*/
class platform_reactor : public capy::service
{
public:
    /** Submit a work item for later execution. */
    virtual void submit(capy::executor_work* w) = 0;

    /** Process all pending work items until the queue is empty. */
    virtual void process() = 0;
};

//----------------------------------------------------------

namespace detail {

/** Empty mutex type for single-threaded use. */
struct null_mutex
{
    void lock() noexcept {}
    void unlock() noexcept {}
};

} // namespace detail

//----------------------------------------------------------

/** Platform reactor implementation template.

    @tparam UseMutex If true, uses std::mutex for thread safety.
                     If false, uses a no-op mutex for single-threaded use.

    @see platform_reactor_single
    @see platform_reactor_multi
*/
template<bool UseMutex>
class platform_reactor_impl : public platform_reactor
{
    using mutex_type = std::conditional_t<UseMutex, std::mutex, detail::null_mutex>;

public:
    explicit platform_reactor_impl(capy::service_provider&) {}

    void shutdown() override
    {
        std::lock_guard<mutex_type> lock(mutex_);
        while(!q_.empty())
        {
            q_.pop()->destroy();
        }
    }

    void submit(capy::executor_work* w) override
    {
        std::lock_guard<mutex_type> lock(mutex_);
        q_.push(w);
    }

    void process() override
    {
        if constexpr(UseMutex)
            process_multi();
        else
            process_single();
    }

private:
    void process_single()
    {
        struct guard
        {
            capy::executor_work* w = nullptr;

            ~guard()
            {
                if(w)
                    w->destroy();
            }

            void commit() noexcept { w = nullptr; }
        };

        if(q_.empty())
            return;

        guard g;

        while(!q_.empty())
        {
            g.w = q_.pop();
            (*g.w)(); // destroys itself
        }

        g.commit();
    }

    void process_multi()
    {
        struct guard
        {
            platform_reactor_impl* self_;
            capy::executor_work_queue* q_;
            capy::executor_work* w = nullptr;

            guard(platform_reactor_impl* self, capy::executor_work_queue* q) : self_(self), q_(q) {}

            ~guard()
            {
                if(w)
                    w->destroy();
                if(q_ && !q_->empty())
                {
                    // don't lose handlers on exception
                    std::lock_guard<mutex_type> lock(self_->mutex_);
                    self_->q_.push(*q_);
                }
            }

            void commit() noexcept
            {
                q_ = nullptr;
                w = nullptr;
            }
        };

        capy::executor_work_queue q;
        {
            std::lock_guard<mutex_type> lock(mutex_);
            q.push(q_);
        }
        if(q.empty())
            return;

        guard g(this, &q);

        for(;;)
        {
            while(!q.empty())
            {
                g.w = q.pop();
                (*g.w)(); // destroys itself
            }

            // Steal any newly submitted work
            {
                std::lock_guard<mutex_type> lock(mutex_);
                if(q_.empty())
                    break;
                q.push(q_);
            }
        }

        g.commit();
    }

    mutable mutex_type mutex_;
    capy::executor_work_queue q_;
};

//----------------------------------------------------------

/** Platform reactor optimized for single-threaded use.

    This implementation does not use any synchronization primitives
    and provides the best performance for single-threaded applications.

    @note Register with service_provider using make_service or use_service.
          Can be found via find_service<platform_reactor> due to key_type.
*/
class platform_reactor_single : public platform_reactor_impl<false>
{
public:
    using key_type = platform_reactor;

    explicit platform_reactor_single(capy::service_provider& sp) : platform_reactor_impl<false>(sp)
    {}
};

//----------------------------------------------------------

/** Thread-safe platform reactor for multi-threaded use.

    This implementation uses a mutex to protect the work queue,
    allowing safe submission from multiple threads.

    @note Register with service_provider using make_service or use_service.
          Can be found via find_service<platform_reactor> due to key_type.
*/
class platform_reactor_multi : public platform_reactor_impl<true>
{
public:
    using key_type = platform_reactor;

    explicit platform_reactor_multi(capy::service_provider& sp) : platform_reactor_impl<true>(sp) {}
};

} // namespace corosio
} // namespace boost

#endif
