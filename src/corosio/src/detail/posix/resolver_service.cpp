//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/config_backend.hpp"

// This implementation works for all POSIX backends (epoll, kqueue, io_uring, poll)
#if !defined(BOOST_COROSIO_BACKEND_IOCP)

#include "src/detail/posix/resolver_service.hpp"
#include "src/detail/endpoint_convert.hpp"
#include "src/detail/intrusive.hpp"
#include "src/detail/scheduler_op.hpp"

#include <boost/corosio/detail/scheduler.hpp>
#include <boost/corosio/resolver_results.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/coro.hpp>
#include <boost/capy/error.hpp>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/*
    POSIX Resolver Implementation
    =============================

    This file implements async DNS resolution for POSIX backends using a
    thread-per-resolution approach. See resolver_service.hpp for the design
    rationale.

    Class Hierarchy
    ---------------
    - posix_resolver_service (abstract base in header)
    - posix_resolver_service_impl (concrete, defined here)
        - Owns all posix_resolver_impl instances via shared_ptr
        - Stores scheduler* for posting completions
    - posix_resolver_impl (one per resolver object)
        - Contains embedded posix_resolve_op for reuse
        - Uses shared_from_this to prevent premature destruction
    - posix_resolve_op (operation state)
        - Inherits scheduler_op for completion posting
        - Stores copies of host/service (worker thread reads them)

    Worker Thread Lifetime
    ----------------------
    Each resolve() spawns a detached thread. The thread captures a shared_ptr
    to posix_resolver_impl, ensuring the impl (and its embedded op_) stays
    alive until the thread completes, even if the resolver is destroyed.

    Completion Flow
    ---------------
    1. resolve() sets up op_, spawns worker thread
    2. Worker runs getaddrinfo() (blocking)
    3. Worker stores results in op_.stored_results
    4. Worker calls svc_.post(&op_) to queue completion
    5. Scheduler invokes op_() which resumes the coroutine

    Single-Inflight Constraint
    --------------------------
    Each resolver has ONE embedded op_. Concurrent resolve() calls on the
    same resolver would corrupt op_ state. This is documented but not
    enforced at runtime. Users must serialize resolve() calls per-resolver.
*/

namespace boost {
namespace corosio {
namespace detail {

namespace {

// Convert resolve_flags to addrinfo ai_flags
int
flags_to_hints(resolve_flags flags)
{
    int hints = 0;

    if ((flags & resolve_flags::passive) != resolve_flags::none)
        hints |= AI_PASSIVE;
    if ((flags & resolve_flags::numeric_host) != resolve_flags::none)
        hints |= AI_NUMERICHOST;
    if ((flags & resolve_flags::numeric_service) != resolve_flags::none)
        hints |= AI_NUMERICSERV;
    if ((flags & resolve_flags::address_configured) != resolve_flags::none)
        hints |= AI_ADDRCONFIG;
    if ((flags & resolve_flags::v4_mapped) != resolve_flags::none)
        hints |= AI_V4MAPPED;
    if ((flags & resolve_flags::all_matching) != resolve_flags::none)
        hints |= AI_ALL;

    return hints;
}

// Convert addrinfo results to resolver_results
resolver_results
convert_results(
    struct addrinfo* ai,
    std::string_view host,
    std::string_view service)
{
    std::vector<resolver_entry> entries;
    entries.reserve(4);  // Most lookups return 1-4 addresses

    for (auto* p = ai; p != nullptr; p = p->ai_next)
    {
        if (p->ai_family == AF_INET)
        {
            auto* addr = reinterpret_cast<sockaddr_in*>(p->ai_addr);
            auto ep = from_sockaddr_in(*addr);
            entries.emplace_back(ep, host, service);
        }
        else if (p->ai_family == AF_INET6)
        {
            auto* addr = reinterpret_cast<sockaddr_in6*>(p->ai_addr);
            auto ep = from_sockaddr_in6(*addr);
            entries.emplace_back(ep, host, service);
        }
    }

    return resolver_results(std::move(entries));
}

// Convert getaddrinfo error codes to system::error_code
system::error_code
make_gai_error(int gai_err)
{
    // Map GAI errors to appropriate generic error codes
    switch (gai_err)
    {
    case EAI_AGAIN:
        // Temporary failure - try again later
        return system::error_code(
            static_cast<int>(std::errc::resource_unavailable_try_again),
            system::generic_category());

    case EAI_BADFLAGS:
        // Invalid flags
        return system::error_code(
            static_cast<int>(std::errc::invalid_argument),
            system::generic_category());

    case EAI_FAIL:
        // Non-recoverable failure
        return system::error_code(
            static_cast<int>(std::errc::io_error),
            system::generic_category());

    case EAI_FAMILY:
        // Address family not supported
        return system::error_code(
            static_cast<int>(std::errc::address_family_not_supported),
            system::generic_category());

    case EAI_MEMORY:
        // Memory allocation failure
        return system::error_code(
            static_cast<int>(std::errc::not_enough_memory),
            system::generic_category());

    case EAI_NONAME:
        // Host or service not found
        return system::error_code(
            static_cast<int>(std::errc::no_such_device_or_address),
            system::generic_category());

    case EAI_SERVICE:
        // Service not supported for socket type
        return system::error_code(
            static_cast<int>(std::errc::invalid_argument),
            system::generic_category());

    case EAI_SOCKTYPE:
        // Socket type not supported
        return system::error_code(
            static_cast<int>(std::errc::not_supported),
            system::generic_category());

    case EAI_SYSTEM:
        // System error - use errno
        return system::error_code(errno, system::generic_category());

    default:
        // Unknown error
        return system::error_code(
            static_cast<int>(std::errc::io_error),
            system::generic_category());
    }
}

} // anonymous namespace

//------------------------------------------------------------------------------

class posix_resolver_service_impl;

//------------------------------------------------------------------------------
// posix_resolve_op - operation state for a single DNS resolution
//------------------------------------------------------------------------------

struct posix_resolve_op : scheduler_op
{
    struct canceller
    {
        posix_resolve_op* op;
        void operator()() const noexcept { op->request_cancel(); }
    };

    // Coroutine state
    capy::coro h;
    capy::executor_ref d;

    // Output parameters
    system::error_code* ec_out = nullptr;
    resolver_results* out = nullptr;

    // Input parameters (owned copies for thread safety)
    std::string host;
    std::string service;
    resolve_flags flags = resolve_flags::none;

    // Result storage (populated by worker thread)
    resolver_results stored_results;
    int gai_error = 0;

    // Thread coordination
    std::atomic<bool> cancelled{false};
    std::optional<std::stop_callback<canceller>> stop_cb;

    posix_resolve_op()
    {
        data_ = this;
    }

    void reset() noexcept;
    void operator()() override;
    void destroy() override;
    void request_cancel() noexcept;
    void start(std::stop_token token);
};

//------------------------------------------------------------------------------
// posix_resolver_impl - per-resolver implementation
//------------------------------------------------------------------------------

/** Resolver implementation for POSIX backends.

    Each resolver instance contains a single embedded operation object (op_)
    that is reused for each resolve() call. This design avoids per-operation
    heap allocation but imposes a critical constraint:

    @par Single-Inflight Contract

    Only ONE resolve operation may be in progress at a time per resolver
    instance. Calling resolve() while a previous resolve() is still pending
    results in undefined behavior:

    - The new call overwrites op_ fields (host, service, coroutine handle)
    - The worker thread from the first call reads corrupted state
    - The wrong coroutine may be resumed, or resumed multiple times
    - Data races occur on non-atomic op_ members

    @par Safe Usage Patterns

    @code
    // CORRECT: Sequential resolves
    auto [ec1, r1] = co_await resolver.resolve("host1", "80");
    auto [ec2, r2] = co_await resolver.resolve("host2", "80");

    // CORRECT: Parallel resolves with separate resolver instances
    resolver r1(ctx), r2(ctx);
    auto [ec1, res1] = co_await r1.resolve("host1", "80");  // in one coroutine
    auto [ec2, res2] = co_await r2.resolve("host2", "80");  // in another

    // WRONG: Concurrent resolves on same resolver
    // These may run concurrently if launched in parallel - UNDEFINED BEHAVIOR
    auto f1 = resolver.resolve("host1", "80");
    auto f2 = resolver.resolve("host2", "80");  // BAD: overlaps with f1
    @endcode

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Unsafe. See single-inflight contract above.
*/
class posix_resolver_impl
    : public resolver::resolver_impl
    , public std::enable_shared_from_this<posix_resolver_impl>
    , public intrusive_list<posix_resolver_impl>::node
{
    friend class posix_resolver_service_impl;

public:
    explicit posix_resolver_impl(posix_resolver_service_impl& svc) noexcept
        : svc_(svc)
    {
    }

    void release() override;

    void resolve(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::string_view host,
        std::string_view service,
        resolve_flags flags,
        std::stop_token,
        system::error_code*,
        resolver_results*) override;

    void cancel() noexcept override;

    posix_resolve_op op_;

private:
    posix_resolver_service_impl& svc_;
};

//------------------------------------------------------------------------------
// posix_resolver_service_impl - concrete service implementation
//------------------------------------------------------------------------------

class posix_resolver_service_impl : public posix_resolver_service
{
public:
    using key_type = posix_resolver_service;

    posix_resolver_service_impl(
        capy::execution_context&,
        scheduler& sched)
        : sched_(&sched)
    {
    }

    ~posix_resolver_service_impl()
    {
    }

    posix_resolver_service_impl(posix_resolver_service_impl const&) = delete;
    posix_resolver_service_impl& operator=(posix_resolver_service_impl const&) = delete;

    void shutdown() override;
    resolver::resolver_impl& create_impl() override;
    void destroy_impl(posix_resolver_impl& impl);

    void post(scheduler_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

    // Thread tracking for safe shutdown
    void thread_started() noexcept;
    void thread_finished() noexcept;
    bool is_shutting_down() const noexcept;

private:
    scheduler* sched_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> shutting_down_{false};
    std::size_t active_threads_ = 0;
    intrusive_list<posix_resolver_impl> resolver_list_;
    std::unordered_map<posix_resolver_impl*,
        std::shared_ptr<posix_resolver_impl>> resolver_ptrs_;
};

//------------------------------------------------------------------------------
// posix_resolve_op implementation
//------------------------------------------------------------------------------

void
posix_resolve_op::
reset() noexcept
{
    host.clear();
    service.clear();
    flags = resolve_flags::none;
    stored_results = resolver_results{};
    gai_error = 0;
    cancelled.store(false, std::memory_order_relaxed);
    stop_cb.reset();
    ec_out = nullptr;
    out = nullptr;
}

void
posix_resolve_op::
operator()()
{
    stop_cb.reset();  // Disconnect stop callback

    bool const was_cancelled = cancelled.load(std::memory_order_acquire);

    if (ec_out)
    {
        if (was_cancelled)
            *ec_out = capy::error::canceled;
        else if (gai_error != 0)
            *ec_out = make_gai_error(gai_error);
        else
            *ec_out = {};  // Clear on success
    }

    if (out && !was_cancelled && gai_error == 0)
        *out = std::move(stored_results);

    d.dispatch(h).resume();
}

void
posix_resolve_op::
destroy()
{
    stop_cb.reset();
}

void
posix_resolve_op::
request_cancel() noexcept
{
    cancelled.store(true, std::memory_order_release);
}

void
posix_resolve_op::
start(std::stop_token token)
{
    cancelled.store(false, std::memory_order_release);
    stop_cb.reset();

    if (token.stop_possible())
        stop_cb.emplace(token, canceller{this});
}

//------------------------------------------------------------------------------
// posix_resolver_impl implementation
//------------------------------------------------------------------------------

void
posix_resolver_impl::
release()
{
    cancel();
    svc_.destroy_impl(*this);
}

void
posix_resolver_impl::
resolve(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    std::string_view host,
    std::string_view service,
    resolve_flags flags,
    std::stop_token token,
    system::error_code* ec,
    resolver_results* out)
{
    auto& op = op_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.out = out;
    op.host = host;
    op.service = service;
    op.flags = flags;
    op.start(token);

    // Keep io_context alive while resolution is pending
    svc_.work_started();

    // Track thread for safe shutdown
    svc_.thread_started();

    try
    {
        // Prevent impl destruction while worker thread is running
        auto self = this->shared_from_this();
        std::thread worker([this, self = std::move(self)]() {
            struct addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = flags_to_hints(op_.flags);

            struct addrinfo* ai = nullptr;
            int result = ::getaddrinfo(
                op_.host.empty() ? nullptr : op_.host.c_str(),
                op_.service.empty() ? nullptr : op_.service.c_str(),
                &hints, &ai);

            if (!op_.cancelled.load(std::memory_order_acquire))
            {
                if (result == 0 && ai)
                {
                    op_.stored_results = convert_results(ai, op_.host, op_.service);
                    op_.gai_error = 0;
                }
                else
                {
                    op_.gai_error = result;
                }
            }

            if (ai)
                ::freeaddrinfo(ai);

            // Only access service methods if not shutting down
            // (service may be destroyed during shutdown)
            if (!svc_.is_shutting_down())
            {
                svc_.work_finished();
                svc_.post(&op_);
            }

            // Signal thread completion for shutdown synchronization
            svc_.thread_finished();
        });
        worker.detach();
    }
    catch (std::system_error const&)
    {
        // Thread creation failed - no thread was started
        svc_.thread_finished();

        // Set error and post completion to avoid hanging the coroutine
        op_.gai_error = EAI_MEMORY;  // Map to "not enough memory"
        svc_.work_finished();
        svc_.post(&op_);
    }
}

void
posix_resolver_impl::
cancel() noexcept
{
    op_.request_cancel();
}

//------------------------------------------------------------------------------
// posix_resolver_service_impl implementation
//------------------------------------------------------------------------------

void
posix_resolver_service_impl::
shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Signal threads to not access service after getaddrinfo returns
        shutting_down_.store(true, std::memory_order_release);

        // Cancel all resolvers (sets cancelled flag checked by threads)
        for (auto* impl = resolver_list_.pop_front(); impl != nullptr;
             impl = resolver_list_.pop_front())
        {
            impl->cancel();
        }

        // Clear the map which releases shared_ptrs
        resolver_ptrs_.clear();
    }

    // Wait for all worker threads to finish before service is destroyed
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return active_threads_ == 0; });
    }
}

resolver::resolver_impl&
posix_resolver_service_impl::
create_impl()
{
    auto ptr = std::make_shared<posix_resolver_impl>(*this);
    auto* impl = ptr.get();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        resolver_list_.push_back(impl);
        resolver_ptrs_[impl] = std::move(ptr);
    }

    return *impl;
}

void
posix_resolver_service_impl::
destroy_impl(posix_resolver_impl& impl)
{
    std::lock_guard<std::mutex> lock(mutex_);
    resolver_list_.remove(&impl);
    resolver_ptrs_.erase(&impl);
}

void
posix_resolver_service_impl::
post(scheduler_op* op)
{
    sched_->post(op);
}

void
posix_resolver_service_impl::
work_started() noexcept
{
    sched_->work_started();
}

void
posix_resolver_service_impl::
work_finished() noexcept
{
    sched_->work_finished();
}

void
posix_resolver_service_impl::
thread_started() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++active_threads_;
}

void
posix_resolver_service_impl::
thread_finished() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    --active_threads_;
    cv_.notify_one();
}

bool
posix_resolver_service_impl::
is_shutting_down() const noexcept
{
    return shutting_down_.load(std::memory_order_acquire);
}

//------------------------------------------------------------------------------
// Free function to get/create the resolver service
//------------------------------------------------------------------------------

posix_resolver_service&
get_resolver_service(capy::execution_context& ctx, scheduler& sched)
{
    return ctx.make_service<posix_resolver_service_impl>(sched);
}

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // !BOOST_COROSIO_BACKEND_IOCP
