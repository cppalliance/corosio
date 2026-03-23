//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_RESOLVER_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_RESOLVER_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/corosio/detail/thread_pool.hpp>

#include <boost/corosio/detail/scheduler.hpp>
#include <boost/corosio/resolver_results.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <coroutine>
#include <boost/capy/error.hpp>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <atomic>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

/*
    POSIX Resolver Service
    ======================

    POSIX getaddrinfo() is a blocking call that cannot be monitored with
    epoll/kqueue/io_uring. Blocking calls are dispatched to a shared
    resolver_thread_pool service which reuses threads across operations.

    Cancellation
    ------------
    getaddrinfo() cannot be interrupted mid-call. We use an atomic flag to
    indicate cancellation was requested. The worker thread checks this flag
    after getaddrinfo() returns and reports the appropriate error.

    Class Hierarchy
    ---------------
    - posix_resolver_service (execution_context service, one per context)
        - Owns all posix_resolver instances via shared_ptr
        - Stores scheduler* for posting completions
    - posix_resolver (one per resolver object)
        - Contains embedded resolve_op and reverse_resolve_op for reuse
        - Uses shared_from_this to prevent premature destruction
    - resolve_op (forward resolution state)
        - Uses getaddrinfo() to resolve host/service to endpoints
    - reverse_resolve_op (reverse resolution state)
        - Uses getnameinfo() to resolve endpoint to host/service

    Completion Flow
    ---------------
    Forward resolution:
    1. resolve() sets up op_, posts work to the thread pool
    2. Pool thread runs getaddrinfo() (blocking)
    3. Pool thread stores results in op_.stored_results
    4. Pool thread calls svc_.post(&op_) to queue completion
    5. Scheduler invokes op_() which resumes the coroutine

    Reverse resolution follows the same pattern using getnameinfo().

    Single-Inflight Constraint
    --------------------------
    Each resolver has ONE embedded op_ for forward and ONE reverse_op_ for
    reverse resolution. Concurrent operations of the same type on the same
    resolver would corrupt state. Users must serialize operations per-resolver.

    Shutdown
    --------
    The resolver service cancels all resolvers and clears the impl map.
    The thread pool service shuts down separately via execution_context
    service ordering, joining all worker threads.
*/

namespace boost::corosio::detail {

struct scheduler;

namespace posix_resolver_detail {

// Convert resolve_flags to addrinfo ai_flags
int flags_to_hints(resolve_flags flags);

// Convert reverse_flags to getnameinfo NI_* flags
int flags_to_ni_flags(reverse_flags flags);

// Convert addrinfo results to resolver_results
resolver_results convert_results(
    struct addrinfo* ai, std::string_view host, std::string_view service);

// Convert getaddrinfo error codes to std::error_code
std::error_code make_gai_error(int gai_err);

} // namespace posix_resolver_detail

class posix_resolver_service;

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
class posix_resolver final
    : public resolver::implementation
    , public std::enable_shared_from_this<posix_resolver>
    , public intrusive_list<posix_resolver>::node
{
    friend class posix_resolver_service;

public:
    // resolve_op - operation state for a single DNS resolution

    struct resolve_op : scheduler_op
    {
        struct canceller
        {
            resolve_op* op;
            void operator()() const noexcept
            {
                op->request_cancel();
            }
        };

        // Coroutine state
        std::coroutine_handle<> h;
        detail::continuation_op cont_op;
        capy::executor_ref ex;
        posix_resolver* impl = nullptr;

        // Output parameters
        std::error_code* ec_out = nullptr;
        resolver_results* out   = nullptr;

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

        resolve_op() = default;

        void reset() noexcept;
        void operator()() override;
        void destroy() override;
        void request_cancel() noexcept;
        void start(std::stop_token const& token);
    };

    // reverse_resolve_op - operation state for reverse DNS resolution

    struct reverse_resolve_op : scheduler_op
    {
        struct canceller
        {
            reverse_resolve_op* op;
            void operator()() const noexcept
            {
                op->request_cancel();
            }
        };

        // Coroutine state
        std::coroutine_handle<> h;
        detail::continuation_op cont_op;
        capy::executor_ref ex;
        posix_resolver* impl = nullptr;

        // Output parameters
        std::error_code* ec_out             = nullptr;
        reverse_resolver_result* result_out = nullptr;

        // Input parameters
        endpoint ep;
        reverse_flags flags = reverse_flags::none;

        // Result storage (populated by worker thread)
        std::string stored_host;
        std::string stored_service;
        int gai_error = 0;

        // Thread coordination
        std::atomic<bool> cancelled{false};
        std::optional<std::stop_callback<canceller>> stop_cb;

        reverse_resolve_op() = default;

        void reset() noexcept;
        void operator()() override;
        void destroy() override;
        void request_cancel() noexcept;
        void start(std::stop_token const& token);
    };

    /// Embedded pool work item for thread pool dispatch.
    struct pool_op : pool_work_item
    {
        /// Resolver that owns this work item.
        posix_resolver* resolver_ = nullptr;

        /// Prevent impl destruction while work is in flight.
        std::shared_ptr<posix_resolver> ref_;
    };

    explicit posix_resolver(posix_resolver_service& svc) noexcept;

    std::coroutine_handle<> resolve(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::string_view host,
        std::string_view service,
        resolve_flags flags,
        std::stop_token,
        std::error_code*,
        resolver_results*) override;

    std::coroutine_handle<> reverse_resolve(
        std::coroutine_handle<>,
        capy::executor_ref,
        endpoint const& ep,
        reverse_flags flags,
        std::stop_token,
        std::error_code*,
        reverse_resolver_result*) override;

    void cancel() noexcept override;

    resolve_op op_;
    reverse_resolve_op reverse_op_;

    /// Pool work item for forward resolution.
    pool_op resolve_pool_op_;

    /// Pool work item for reverse resolution.
    pool_op reverse_pool_op_;

    /// Execute blocking `getaddrinfo()` on a pool thread.
    static void do_resolve_work(pool_work_item*) noexcept;

    /// Execute blocking `getnameinfo()` on a pool thread.
    static void do_reverse_resolve_work(pool_work_item*) noexcept;

private:
    posix_resolver_service& svc_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_RESOLVER_HPP
