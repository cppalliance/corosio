//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_WIN_IOCP_RESOLVER_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_WIN_IOCP_RESOLVER_SERVICE_HPP

#include <boost/corosio/detail/config.hpp>

#ifdef _WIN32

// GetAddrInfoExW requires Windows 8 or later
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0602)
#error "corosio resolver requires Windows 8 or later (_WIN32_WINNT >= 0x0602)"
#endif

#include <boost/corosio/resolver.hpp>
#include <boost/corosio/resolver_results.hpp>
#include <boost/capy/ex/any_dispatcher.hpp>
#include <boost/capy/concept/affine_awaitable.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/core/intrusive_list.hpp>

#include "detail/windows.hpp"
#include "detail/win_overlapped_op.hpp"
#include "detail/win_mutex.hpp"
#include "detail/win_wsa_init.hpp"

#include <WS2tcpip.h>

#include <string>

namespace boost {
namespace corosio {
namespace detail {

class win_iocp_scheduler;
class win_iocp_resolver_service;
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
        DWORD error,
        DWORD bytes,
        OVERLAPPED* ov);

    /** Resume the coroutine after resolve completes. */
    void operator()() override;

    void destroy() override;
};

//------------------------------------------------------------------------------

/** Resolver implementation for IOCP-based async DNS.

    This class contains the state for a single resolver, including
    the pending resolve operation.

    @note Internal implementation detail. Users interact with resolver class.
*/
class win_resolver_impl
    : public resolver::resolver_impl
    , public capy::intrusive_list<win_resolver_impl>::node
{
    friend class win_iocp_resolver_service;
    friend struct resolve_op;

public:
    explicit win_resolver_impl(win_iocp_resolver_service& svc) noexcept;

    void release() override;

    void resolve(
        std::coroutine_handle<>,
        capy::any_dispatcher,
        std::string_view host,
        std::string_view service,
        resolve_flags flags,
        std::stop_token,
        system::error_code*,
        resolver_results*) override;

    void cancel() noexcept;

    resolve_op op_;

private:
    win_iocp_resolver_service& svc_;
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
class win_iocp_resolver_service
    : private win_wsa_init
    , public capy::execution_context::service
{
public:
    using key_type = win_iocp_resolver_service;

    /** Construct the resolver service.

        @param ctx Reference to the owning execution_context.
    */
    explicit win_iocp_resolver_service(capy::execution_context& ctx);

    /** Destroy the resolver service. */
    ~win_iocp_resolver_service();

    win_iocp_resolver_service(win_iocp_resolver_service const&) = delete;
    win_iocp_resolver_service& operator=(win_iocp_resolver_service const&) = delete;

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

private:
    win_iocp_scheduler& sched_;
    win_mutex mutex_;
    capy::intrusive_list<win_resolver_impl> resolver_list_;
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // _WIN32

#endif
