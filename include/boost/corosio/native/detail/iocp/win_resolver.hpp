//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_RESOLVER_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_RESOLVER_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

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
#include <boost/corosio/detail/intrusive.hpp>

#include <boost/corosio/native/detail/iocp/win_windows.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_mutex.hpp>
#include <boost/corosio/native/detail/iocp/win_wsa_init.hpp>

#include <boost/corosio/detail/endpoint_convert.hpp>
#include <boost/corosio/detail/make_err.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>

#include <WS2tcpip.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

// MinGW may not have GetAddrInfoExCancel declared
#if defined(__MINGW32__) || defined(__MINGW64__)
extern "C"
{
    INT WSAAPI GetAddrInfoExCancel(LPHANDLE lpHandle);
}
#endif

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
        - Owns all win_resolver instances via shared_ptr
        - Coordinates with win_scheduler for work tracking
        - Tracks active worker threads for safe shutdown
    - win_resolver (one per resolver object)
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

/*
    Windows IOCP Resolver Implementation
    ====================================

    See above for architecture overview.

    Forward Resolution (GetAddrInfoExW)
    -----------------------------------
    1. resolve() converts host/service to wide strings (Windows API requirement)
    2. GetAddrInfoExW() is called with our completion callback
    3. If it returns WSA_IO_PENDING, completion comes later via callback
    4. If it returns immediately (0 or error), we post completion manually
    5. completion() callback stores error, calls work_finished(), posts op
    6. op_() resumes the coroutine with results or error

    Reverse Resolution (GetNameInfoW)
    ---------------------------------
    Unlike GetAddrInfoExW, GetNameInfoW has no async variant. We use a worker
    thread approach similar to POSIX:
    1. reverse_resolve() spawns a detached worker thread
    2. Worker calls GetNameInfoW() (blocking)
    3. Worker converts wide results to UTF-8 via WideCharToMultiByte
    4. Worker posts completion to scheduler
    5. op_() resumes the coroutine with results

    Thread tracking (thread_started/thread_finished) ensures safe shutdown
    by waiting for all worker threads before destroying the service.

    String Conversion
    -----------------
    Windows APIs require wide strings. We use MultiByteToWideChar for
    UTF-8 to UTF-16 and WideCharToMultiByte for UTF-16 to UTF-8.

    Work Tracking
    -------------
    work_started() is called before async operations to keep io_context alive.
    work_finished() is called when the operation completes (in callback for
    forward resolution, in worker thread for reverse resolution).
*/

namespace boost::corosio::detail {

class win_resolver_service;
class win_resolver;

/** Resolve operation state. */
struct resolve_op : overlapped_op
{
    ADDRINFOEXW* results  = nullptr;
    HANDLE cancel_handle  = nullptr;
    resolver_results* out = nullptr;
    std::string host;
    std::string service;
    std::wstring host_w;
    std::wstring service_w;
    win_resolver* impl = nullptr;

    /** Completion callback for GetAddrInfoExW. */
    static void CALLBACK completion(DWORD dwError, DWORD bytes, OVERLAPPED* ov);

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);

    resolve_op() noexcept;
};

/** Reverse resolve operation state. */
struct reverse_resolve_op : overlapped_op
{
    reverse_resolver_result* result_out = nullptr;
    endpoint ep;
    reverse_flags flags = reverse_flags::none;
    std::string stored_host;
    std::string stored_service;
    int gai_error      = 0;
    win_resolver* impl = nullptr;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);

    reverse_resolve_op() noexcept;
};

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
class win_resolver final
    : public resolver::implementation
    , public std::enable_shared_from_this<win_resolver>
    , public intrusive_list<win_resolver>::node
{
    friend class win_resolver_service;
    friend struct resolve_op;

public:
    explicit win_resolver(win_resolver_service& svc) noexcept;

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

private:
    win_resolver_service& svc_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_RESOLVER_HPP
