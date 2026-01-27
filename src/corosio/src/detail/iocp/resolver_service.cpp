//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_IOCP)

#include "src/detail/iocp/resolver_service.hpp"
#include "src/detail/iocp/scheduler.hpp"
#include "src/detail/endpoint_convert.hpp"
#include "src/detail/make_err.hpp"

#include <boost/url/ipv4_address.hpp>
#include <boost/url/ipv6_address.hpp>

#include <cstring>
#include <thread>

// MinGW may not have GetAddrInfoExCancel declared
#if defined(__MINGW32__) || defined(__MINGW64__)
extern "C" {
INT WSAAPI GetAddrInfoExCancel(LPHANDLE lpHandle);
}
#endif

/*
    Windows IOCP Resolver Implementation
    ====================================

    See resolver_service.hpp for architecture overview.

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

namespace {

// Convert narrow string to wide string
std::wstring
to_wide(std::string_view s)
{
    if (s.empty())
        return {};

    int len = ::MultiByteToWideChar(
        CP_UTF8, 0,
        s.data(), static_cast<int>(s.size()),
        nullptr, 0);

    if (len <= 0)
        return {};

    std::wstring result(static_cast<std::size_t>(len), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0,
        s.data(), static_cast<int>(s.size()),
        result.data(), len);

    return result;
}

// Convert resolve_flags to ADDRINFOEXW hints
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

// Convert reverse_flags to getnameinfo NI_* flags
int
flags_to_ni_flags(reverse_flags flags)
{
    int ni_flags = 0;

    if ((flags & reverse_flags::numeric_host) != reverse_flags::none)
        ni_flags |= NI_NUMERICHOST;
    if ((flags & reverse_flags::numeric_service) != reverse_flags::none)
        ni_flags |= NI_NUMERICSERV;
    if ((flags & reverse_flags::name_required) != reverse_flags::none)
        ni_flags |= NI_NAMEREQD;
    if ((flags & reverse_flags::datagram_service) != reverse_flags::none)
        ni_flags |= NI_DGRAM;

    return ni_flags;
}

// Convert wide string to UTF-8 string
std::string
from_wide(std::wstring_view s)
{
    if (s.empty())
        return {};

    int len = ::WideCharToMultiByte(
        CP_UTF8, 0,
        s.data(), static_cast<int>(s.size()),
        nullptr, 0,
        nullptr, nullptr);

    if (len <= 0)
        return {};

    std::string result(static_cast<std::size_t>(len), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0,
        s.data(), static_cast<int>(s.size()),
        result.data(), len,
        nullptr, nullptr);

    return result;
}

// Convert ADDRINFOEXW results to resolver_results
resolver_results
convert_results(
    ADDRINFOEXW* ai,
    std::string_view host,
    std::string_view service)
{
    std::vector<resolver_entry> entries;

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

} // namespace

//------------------------------------------------------------------------------
// resolve_op
//------------------------------------------------------------------------------

void CALLBACK
resolve_op::
completion(
    DWORD dwError,
    DWORD /*bytes*/,
    OVERLAPPED* ov)
{
    auto* op = static_cast<resolve_op*>(ov);
    op->dwError = dwError;
    op->impl->svc_.work_finished();
    op->impl->svc_.post(op);
}

void
resolve_op::
operator()()
{
    stop_cb.reset();

    if (ec_out)
    {
        if (cancelled.load(std::memory_order_acquire))
            *ec_out = capy::error::canceled;
        else if (dwError != 0)
            *ec_out = make_err(dwError);
        else
            *ec_out = {};  // Clear on success
    }

    if (out && !cancelled.load(std::memory_order_acquire) && dwError == 0 && results)
    {
        *out = convert_results(results, host, service);
    }

    if (results)
    {
        ::FreeAddrInfoExW(results);
        results = nullptr;
    }

    cancel_handle = nullptr;

    d.dispatch(h).resume();
}

void
resolve_op::
destroy()
{
    stop_cb.reset();

    if (results)
    {
        ::FreeAddrInfoExW(results);
        results = nullptr;
    }

    cancel_handle = nullptr;
}

//------------------------------------------------------------------------------
// reverse_resolve_op
//------------------------------------------------------------------------------

void
reverse_resolve_op::
operator()()
{
    stop_cb.reset();

    if (ec_out)
    {
        if (cancelled.load(std::memory_order_acquire))
            *ec_out = capy::error::canceled;
        else if (gai_error != 0)
            *ec_out = make_err(static_cast<DWORD>(gai_error));
        else
            *ec_out = {};  // Clear on success
    }

    if (result_out && !cancelled.load(std::memory_order_acquire) && gai_error == 0)
    {
        *result_out = reverse_resolver_result(
            ep, std::move(stored_host), std::move(stored_service));
    }

    d.dispatch(h).resume();
}

void
reverse_resolve_op::
destroy()
{
    stop_cb.reset();
}

//------------------------------------------------------------------------------
// win_resolver_impl
//------------------------------------------------------------------------------

win_resolver_impl::
win_resolver_impl(win_resolver_service& svc) noexcept
    : svc_(svc)
{
}

void
win_resolver_impl::
release()
{
    cancel();
    svc_.destroy_impl(*this);
}

void
win_resolver_impl::
resolve(
    capy::coro h,
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
    op.impl = this;
    op.host = host;
    op.service = service;
    op.host_w = to_wide(host);
    op.service_w = to_wide(service);
    op.start(token);

    ADDRINFOEXW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = flags_to_hints(flags);

    // Keep io_context alive while resolution is pending
    svc_.work_started();

    int result = ::GetAddrInfoExW(
        op.host_w.empty() ? nullptr : op.host_w.c_str(),
        op.service_w.empty() ? nullptr : op.service_w.c_str(),
        NS_DNS,
        nullptr,
        &hints,
        &op.results,
        nullptr,
        &op,
        &resolve_op::completion,
        &op.cancel_handle);

    if (result != WSA_IO_PENDING)
    {
        // Completed synchronously - callback won't be invoked
        svc_.work_finished();

        if (result == 0)
        {
            // Completed synchronously
            op.dwError = 0;
        }
        else
        {
            op.dwError = static_cast<DWORD>(::WSAGetLastError());
        }

        svc_.post(&op);
    }
}

void
win_resolver_impl::
reverse_resolve(
    capy::coro h,
    capy::executor_ref d,
    endpoint const& ep,
    reverse_flags flags,
    std::stop_token token,
    system::error_code* ec,
    reverse_resolver_result* result_out)
{
    auto& op = reverse_op_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.result_out = result_out;
    op.impl = this;
    op.ep = ep;
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

        // GetNameInfoW is synchronous, so we need to use a thread
        std::thread worker([this, self = std::move(self)]() {
            // Build sockaddr from endpoint
            sockaddr_storage ss{};
            int ss_len;

            if (reverse_op_.ep.is_v4())
            {
                auto sa = to_sockaddr_in(reverse_op_.ep);
                std::memcpy(&ss, &sa, sizeof(sa));
                ss_len = sizeof(sockaddr_in);
            }
            else
            {
                auto sa = to_sockaddr_in6(reverse_op_.ep);
                std::memcpy(&ss, &sa, sizeof(sa));
                ss_len = sizeof(sockaddr_in6);
            }

            wchar_t host[NI_MAXHOST];
            wchar_t service[NI_MAXSERV];

            int result = ::GetNameInfoW(
                reinterpret_cast<sockaddr*>(&ss), ss_len,
                host, NI_MAXHOST,
                service, NI_MAXSERV,
                flags_to_ni_flags(reverse_op_.flags));

            if (!reverse_op_.cancelled.load(std::memory_order_acquire))
            {
                if (result == 0)
                {
                    reverse_op_.stored_host = from_wide(host);
                    reverse_op_.stored_service = from_wide(service);
                    reverse_op_.gai_error = 0;
                }
                else
                {
                    reverse_op_.gai_error = result;
                }
            }

            // Always post so the scheduler can properly drain the op
            // during shutdown via destroy().
            svc_.work_finished();
            svc_.post(&reverse_op_);

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
        svc_.work_finished();
        reverse_op_.gai_error = WSAENOBUFS;  // Map to "not enough memory"
        svc_.post(&reverse_op_);
    }
}

void
win_resolver_impl::
cancel() noexcept
{
    op_.request_cancel();
    reverse_op_.request_cancel();

    if (op_.cancel_handle)
    {
        ::GetAddrInfoExCancel(&op_.cancel_handle);
    }
}

//------------------------------------------------------------------------------
// win_resolver_service
//------------------------------------------------------------------------------

win_resolver_service::
win_resolver_service(
    capy::execution_context& ctx,
    scheduler& sched)
    : sched_(sched)
{
    (void)ctx;
}

win_resolver_service::
~win_resolver_service()
{
}

void
win_resolver_service::
shutdown()
{
    {
        std::lock_guard<win_mutex> lock(mutex_);

        // Signal threads to not access service after GetNameInfoW returns
        shutting_down_.store(true, std::memory_order_release);

        // Cancel all resolvers (sets cancelled flag checked by threads)
        for (auto* impl = resolver_list_.pop_front(); impl != nullptr;
             impl = resolver_list_.pop_front())
        {
            impl->cancel();
        }

        // Clear the map which releases shared_ptrs
        // Note: impls may still be alive if worker threads hold references
        resolver_ptrs_.clear();
    }

    // Wait for all worker threads to finish before service is destroyed
    {
        std::unique_lock<win_mutex> lock(mutex_);
        cv_.wait(lock, [this] { return active_threads_ == 0; });
    }
}

win_resolver_impl&
win_resolver_service::
create_impl()
{
    auto ptr = std::make_shared<win_resolver_impl>(*this);
    auto* impl = ptr.get();

    {
        std::lock_guard<win_mutex> lock(mutex_);
        resolver_list_.push_back(impl);
        resolver_ptrs_[impl] = std::move(ptr);
    }

    return *impl;
}

void
win_resolver_service::
destroy_impl(win_resolver_impl& impl)
{
    std::lock_guard<win_mutex> lock(mutex_);
    resolver_list_.remove(&impl);
    resolver_ptrs_.erase(&impl);
}

void
win_resolver_service::
post(overlapped_op* op)
{
    sched_.post(op);
}

void
win_resolver_service::
work_started() noexcept
{
    sched_.work_started();
}

void
win_resolver_service::
work_finished() noexcept
{
    sched_.work_finished();
}

void
win_resolver_service::
thread_started() noexcept
{
    std::lock_guard<win_mutex> lock(mutex_);
    ++active_threads_;
}

void
win_resolver_service::
thread_finished() noexcept
{
    std::lock_guard<win_mutex> lock(mutex_);
    --active_threads_;
    cv_.notify_one();
}

bool
win_resolver_service::
is_shutting_down() const noexcept
{
    return shutting_down_.load(std::memory_order_acquire);
}

} // namespace boost::corosio::detail

#endif // _WIN32
