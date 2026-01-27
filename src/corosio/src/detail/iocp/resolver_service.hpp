//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_IOCP_RESOLVER_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_IOCP_RESOLVER_SERVICE_HPP


#if defined(_WIN32)

#include <boost/corosio/detail/config.hpp>

// GetAddrInfoExW requires Windows 8 or later
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0602)
#error "corosio resolver requires Windows 8 or later (_WIN32_WINNT >= 0x0602)"
#endif

#include <boost/corosio/detail/scheduler.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/resolver_results.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include "src/detail/intrusive.hpp"

#include "src/detail/iocp/windows.hpp"
#include "src/detail/iocp/overlapped_op.hpp"
#include "src/detail/iocp/mutex.hpp"
#include "src/detail/iocp/wsa_init.hpp"

#include <WS2tcpip.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <string>
#include <unordered_map>

/*
    Windows IOCP Resolver Service
    =============================

    This header declares the Windows resolver implementation.

    Forward Resolution (GetAddrInfoExW)
    -----------------------------------
    Uses the native async GetAddrInfoExW API which provides completion
    callbacks that integrate with IOCP. This avoids worker threads for
    forward DNS lookups.

    Reverse Resolution (GetNameInfoW)
    ---------------------------------
    Unlike GetAddrInfoExW, GetNameInfoW has no async variant. Reverse
    resolution spawns a detached worker thread that calls GetNameInfoW
    and posts the result to the scheduler upon completion.

    Class Hierarchy
    ---------------
    - win_resolver_service (execution_context::service)
        - Owns all win_resolver_impl instances via shared_ptr
        - Coordinates with win_scheduler for work tracking
        - Tracks active worker threads for safe shutdown
    - win_resolver_impl (one per resolver object)
        - Contains embedded resolve_op and reverse_resolve_op
        - Inherits from enable_shared_from_this for thread safety
    - resolve_op (overlapped_op subclass)
        - OVERLAPPED base enables IOCP integration
        - Static completion() callback invoked by Windows
    - reverse_resolve_op (overlapped_op subclass)
        - Used by worker thread for reverse resolution

    Shutdown Synchronization
    ------------------------
    The service uses condition_variable_any and win_mutex to track active
    worker threads. During shutdown(), the service waits for all threads
    to complete before destroying resources. Worker threads always post
    their completions so the scheduler can properly drain them via destroy().

    Cancellation
    ------------
    GetAddrInfoExCancel() can cancel in-progress forward resolutions.
    Reverse resolution checks an atomic cancelled flag after GetNameInfoW
    returns. The cancel() method sets flags and calls the Windows cancel API.

    Single-Inflight Constraint
    --------------------------
    Each resolver has ONE embedded op_ for forward resolution and ONE
    reverse_op_ for reverse resolution. Concurrent operations of the same
    type on the same resolver would corrupt state. Users must serialize
    operations per-resolver.
*/

namespace boost::corosio::detail {

class win_resolver_service;
class win_resolver_impl;

//------------------------------------------------------------------------------

/** Resolve operation state. */
struct resolve_op : overlapped_op
{
    ADDRINFOEXW* results = nullptr;
    HANDLE cancel_handle = nullptr;
    resolver_results* out = nullptr;
    std::string host;
    std::string service;
    std::wstring host_w;
    std::wstring service_w;
    win_resolver_impl* impl = nullptr;

    /** Completion callback for GetAddrInfoExW. */
    static void CALLBACK completion(
        DWORD dwError,
        DWORD bytes,
        OVERLAPPED* ov);

    /** Resume the coroutine after resolve completes. */
    void operator()() override;

    void destroy() override;
};

/** Reverse resolve operation state. */
struct reverse_resolve_op : overlapped_op
{
    reverse_resolver_result* result_out = nullptr;
    endpoint ep;
    reverse_flags flags = reverse_flags::none;
    std::string stored_host;
    std::string stored_service;
    int gai_error = 0;
    win_resolver_impl* impl = nullptr;

    /** Resume the coroutine after reverse resolve completes. */
    void operator()() override;

    void destroy() override;
};

//------------------------------------------------------------------------------

/** Resolver implementation for IOCP-based async DNS.

    Each resolver instance contains a single embedded operation object (op_)
    that is reused for each resolve() call. This design avoids per-operation
    heap allocation but imposes a critical constraint:

    @par Single-Inflight Contract

    Only ONE resolve operation may be in progress at a time per resolver
    instance. Calling resolve() while a previous resolve() is still pending
    results in undefined behavior:

    - The new call overwrites op_ fields (host, service, coroutine handle)
    - The pending GetAddrInfoExW callback reads corrupted state
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

    @note Internal implementation detail. Users interact with resolver class.
*/
class win_resolver_impl
    : public resolver::resolver_impl
    , public std::enable_shared_from_this<win_resolver_impl>
    , public intrusive_list<win_resolver_impl>::node
{
    friend class win_resolver_service;
    friend struct resolve_op;

public:
    explicit win_resolver_impl(win_resolver_service& svc) noexcept;

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

    void reverse_resolve(
        std::coroutine_handle<>,
        capy::executor_ref,
        endpoint const& ep,
        reverse_flags flags,
        std::stop_token,
        system::error_code*,
        reverse_resolver_result*) override;

    void cancel() noexcept override;

    resolve_op op_;
    reverse_resolve_op reverse_op_;

private:
    win_resolver_service& svc_;
};

//------------------------------------------------------------------------------

/** Windows IOCP resolver management service.

    This service owns all resolver implementations and coordinates their
    lifecycle. It provides:

    - Resolver implementation allocation and deallocation
    - Async DNS resolution via GetAddrInfoExW
    - Graceful shutdown - destroys all implementations when io_context stops

    @par Thread Safety
    All public member functions are thread-safe.

    @note Only available on Windows platforms with _WIN32_WINNT >= 0x0602.
*/
class win_resolver_service
    : private win_wsa_init
    , public capy::execution_context::service
{
public:
    using key_type = win_resolver_service;

    /** Construct the resolver service.

        @param ctx Reference to the owning execution_context.
        @param sched Reference to the scheduler for posting completions.
    */
    win_resolver_service(capy::execution_context& ctx, scheduler& sched);

    /** Destroy the resolver service. */
    ~win_resolver_service();

    win_resolver_service(win_resolver_service const&) = delete;
    win_resolver_service& operator=(win_resolver_service const&) = delete;

    /** Shut down the service. */
    void shutdown() override;

    /** Create a new resolver implementation. */
    win_resolver_impl& create_impl();

    /** Destroy a resolver implementation. */
    void destroy_impl(win_resolver_impl& impl);

    /** Post an operation for completion. */
    void post(overlapped_op* op);

    /** Notify scheduler of pending I/O work. */
    void work_started() noexcept;

    /** Notify scheduler that I/O work completed. */
    void work_finished() noexcept;

    /** Track worker thread start for safe shutdown. */
    void thread_started() noexcept;

    /** Track worker thread completion for safe shutdown. */
    void thread_finished() noexcept;

    /** Check if service is shutting down. */
    bool is_shutting_down() const noexcept;

private:
    scheduler& sched_;
    win_mutex mutex_;
    std::condition_variable_any cv_;
    std::atomic<bool> shutting_down_{false};
    std::size_t active_threads_ = 0;
    intrusive_list<win_resolver_impl> resolver_list_;
    std::unordered_map<win_resolver_impl*,
        std::shared_ptr<win_resolver_impl>> resolver_ptrs_;
};

} // namespace boost::corosio::detail

#endif // _WIN32

#endif // BOOST_COROSIO_DETAIL_IOCP_RESOLVER_SERVICE_HPP
