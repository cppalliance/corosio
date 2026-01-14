//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifdef _WIN32

#include "detail/win_iocp_resolver_service.hpp"
#include "detail/win_iocp_scheduler.hpp"
#include "detail/endpoint_convert.hpp"

#include <boost/url/ipv4_address.hpp>
#include <boost/url/ipv6_address.hpp>

namespace boost {
namespace corosio {
namespace detail {

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
    DWORD error,
    DWORD /*bytes*/,
    OVERLAPPED* ov)
{
    auto* op = static_cast<resolve_op*>(ov);
    op->error = error;
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
            *ec_out = make_error_code(system::errc::operation_canceled);
        else if (error != 0)
            *ec_out = system::error_code(
                static_cast<int>(error), system::system_category());
    }

    if (out && !cancelled.load(std::memory_order_acquire) && error == 0 && results)
    {
        *out = convert_results(results, host, service);
    }

    if (results)
    {
        ::FreeAddrInfoExW(results);
        results = nullptr;
    }

    cancel_handle = nullptr;

    d(h).resume();
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
// win_resolver_impl
//------------------------------------------------------------------------------

win_resolver_impl::
win_resolver_impl(win_iocp_resolver_service& svc) noexcept
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
    capy::any_coro h,
    capy::any_dispatcher d,
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
        svc_.work_finished();

        if (result == 0)
        {
            // Completed synchronously
            op.error = 0;
        }
        else
        {
            op.error = static_cast<DWORD>(::WSAGetLastError());
        }

        svc_.post(&op);
    }
}

void
win_resolver_impl::
cancel() noexcept
{
    op_.request_cancel();

    if (op_.cancel_handle)
    {
        ::GetAddrInfoExCancel(&op_.cancel_handle);
    }
}

//------------------------------------------------------------------------------
// win_iocp_resolver_service
//------------------------------------------------------------------------------

win_iocp_resolver_service::
win_iocp_resolver_service(
    capy::execution_context& ctx)
    : sched_(ctx.use_service<win_iocp_scheduler>())
{
}

win_iocp_resolver_service::
~win_iocp_resolver_service()
{
}

void
win_iocp_resolver_service::
shutdown()
{
    std::lock_guard<win_mutex> lock(mutex_);

    for (auto* impl = resolver_list_.pop_front(); impl != nullptr;
         impl = resolver_list_.pop_front())
    {
        impl->cancel();
        delete impl;
    }
}

win_resolver_impl&
win_iocp_resolver_service::
create_impl()
{
    auto* impl = new win_resolver_impl(*this);

    {
        std::lock_guard<win_mutex> lock(mutex_);
        resolver_list_.push_back(impl);
    }

    return *impl;
}

void
win_iocp_resolver_service::
destroy_impl(win_resolver_impl& impl)
{
    {
        std::lock_guard<win_mutex> lock(mutex_);
        resolver_list_.remove(&impl);
    }

    delete &impl;
}

void
win_iocp_resolver_service::
post(overlapped_op* op)
{
    sched_.post(op);
}

void
win_iocp_resolver_service::
work_started() noexcept
{
    sched_.work_started();
}

void
win_iocp_resolver_service::
work_finished() noexcept
{
    sched_.work_finished();
}

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // _WIN32
