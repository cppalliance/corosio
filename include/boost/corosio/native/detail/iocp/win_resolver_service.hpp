//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_RESOLVER_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_RESOLVER_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/native/detail/iocp/win_resolver.hpp>
#include <boost/corosio/detail/thread_pool.hpp>

#include <unordered_map>

namespace boost::corosio::detail {

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
BOOST_COROSIO_MSVC_WARNING_PUSH
// 4251: std::/detail:: members; 4275: non-exported win_wsa_init base
BOOST_COROSIO_MSVC_WARNING_DISABLE(4251 4275)
class BOOST_COROSIO_DECL win_resolver_service final
    : private win_wsa_init
    , public capy::execution_context::service
    , public io_object::io_service
{
public:
    using key_type = win_resolver_service;

    io_object::implementation* construct() override;

    void destroy(io_object::implementation* p) override
    {
        auto& impl = static_cast<win_resolver&>(*p);
        impl.cancel();
        destroy_impl(impl);
    }

    /** Construct the resolver service.

        @param ctx Reference to the owning execution_context.
        @param sched Reference to the scheduler for posting completions.
    */
    win_resolver_service(capy::execution_context& ctx, scheduler& sched);

    /** Destroy the resolver service. */
    ~win_resolver_service();

    win_resolver_service(win_resolver_service const&)            = delete;
    win_resolver_service& operator=(win_resolver_service const&) = delete;

    /** Shut down the service. */
    void shutdown() override;

    /** Destroy a resolver implementation. */
    void destroy_impl(win_resolver& impl);

    /** Post an operation for completion. */
    void post(overlapped_op* op);

    /** Notify scheduler of pending I/O work. */
    void work_started() noexcept;

    /** Notify scheduler that I/O work completed. */
    void work_finished() noexcept;

    /** Return the resolver thread pool. */
    thread_pool& pool() noexcept
    {
        return pool_;
    }

private:
    scheduler& sched_;
    thread_pool& pool_;
    win_mutex mutex_;
    intrusive_list<win_resolver> resolver_list_;
    std::unordered_map<win_resolver*, std::shared_ptr<win_resolver>>
        resolver_ptrs_;
};
BOOST_COROSIO_MSVC_WARNING_POP

namespace resolver_detail {

// Convert narrow string to wide string
inline std::wstring
to_wide(std::string_view s)
{
    if (s.empty())
        return {};

    int len = ::MultiByteToWideChar(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);

    if (len <= 0)
        return {};

    std::wstring result(static_cast<std::size_t>(len), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), len);

    return result;
}

// Convert resolve_flags to ADDRINFOEXW hints
inline int
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
inline int
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
inline std::string
from_wide(std::wstring_view s)
{
    if (s.empty())
        return {};

    int len = ::WideCharToMultiByte(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr,
        nullptr);

    if (len <= 0)
        return {};

    std::string result(static_cast<std::size_t>(len), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), len,
        nullptr, nullptr);

    return result;
}

// Convert ADDRINFOEXW results to resolver_results
inline resolver_results
convert_results(
    ADDRINFOEXW* ai, std::string_view host, std::string_view service)
{
    std::vector<resolver_entry> entries;

    for (auto* p = ai; p != nullptr; p = p->ai_next)
    {
        if (p->ai_family == AF_INET)
        {
            auto* addr = reinterpret_cast<sockaddr_in*>(p->ai_addr);
            auto ep    = from_sockaddr_in(*addr);
            entries.emplace_back(ep, host, service);
        }
        else if (p->ai_family == AF_INET6)
        {
            auto* addr = reinterpret_cast<sockaddr_in6*>(p->ai_addr);
            auto ep    = from_sockaddr_in6(*addr);
            entries.emplace_back(ep, host, service);
        }
    }

    return entries;
}

} // namespace resolver_detail

// resolve_op

inline void CALLBACK
resolve_op::completion(DWORD dwError, DWORD /*bytes*/, OVERLAPPED* ov)
{
    auto* op    = static_cast<resolve_op*>(ov);
    op->dwError = dwError;
    op->impl->svc_.work_finished();
    op->impl->svc_.post(op);
}

inline resolve_op::resolve_op() noexcept : overlapped_op(&do_complete) {}

inline void
resolve_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<resolve_op*>(base);

    if (!owner)
    {
        // Destroy path
        op->stop_cb.reset();
        if (op->results)
        {
            ::FreeAddrInfoExW(op->results);
            op->results = nullptr;
        }
        op->cancel_handle = nullptr;
        return;
    }

    op->stop_cb.reset();

    if (op->ec_out)
    {
        if (op->cancelled.load(std::memory_order_acquire))
            *op->ec_out = capy::error::canceled;
        else if (op->dwError != 0)
            *op->ec_out = make_err(op->dwError);
        else
            *op->ec_out = {};
    }

    if (op->out && !op->cancelled.load(std::memory_order_acquire) &&
        op->dwError == 0 && op->results)
    {
        *op->out = resolver_detail::convert_results(
            op->results, op->host, op->service);
    }

    if (op->results)
    {
        ::FreeAddrInfoExW(op->results);
        op->results = nullptr;
    }

    op->cancel_handle = nullptr;

    op->cont_op.cont.h = op->h;
    dispatch_coro(op->ex, op->cont_op.cont).resume();
}

// reverse_resolve_op

inline reverse_resolve_op::reverse_resolve_op() noexcept
    : overlapped_op(&do_complete)
{
}

inline void
reverse_resolve_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<reverse_resolve_op*>(base);

    if (!owner)
    {
        op->stop_cb.reset();
        return;
    }

    op->stop_cb.reset();

    if (op->ec_out)
    {
        if (op->cancelled.load(std::memory_order_acquire))
            *op->ec_out = capy::error::canceled;
        else if (op->gai_error != 0)
            *op->ec_out = make_err(static_cast<DWORD>(op->gai_error));
        else
            *op->ec_out = {};
    }

    if (op->result_out && !op->cancelled.load(std::memory_order_acquire) &&
        op->gai_error == 0)
    {
        *op->result_out = reverse_resolver_result(
            op->ep, std::move(op->stored_host), std::move(op->stored_service));
    }

    op->cont_op.cont.h = op->h;
    dispatch_coro(op->ex, op->cont_op.cont).resume();
}

// win_resolver

inline win_resolver::win_resolver(win_resolver_service& svc) noexcept
    : svc_(svc)
{
}

inline std::coroutine_handle<>
win_resolver::resolve(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    std::string_view host,
    std::string_view service,
    resolve_flags flags,
    std::stop_token token,
    std::error_code* ec,
    resolver_results* out)
{
    auto& op = op_;
    op.reset();
    op.h         = h;
    op.ex        = d;
    op.ec_out    = ec;
    op.out       = out;
    op.impl      = this;
    op.host      = host;
    op.service   = service;
    op.host_w    = resolver_detail::to_wide(host);
    op.service_w = resolver_detail::to_wide(service);
    op.start(token);

    ADDRINFOEXW hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = resolver_detail::flags_to_hints(flags);

    // Keep io_context alive while resolution is pending
    svc_.work_started();

    int result = ::GetAddrInfoExW(
        op.host_w.empty() ? nullptr : op.host_w.c_str(),
        op.service_w.empty() ? nullptr : op.service_w.c_str(), NS_DNS, nullptr,
        &hints, &op.results, nullptr, &op, &resolve_op::completion,
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
    // completion is always posted to scheduler queue, never inline.
    return std::noop_coroutine();
}

inline std::coroutine_handle<>
win_resolver::reverse_resolve(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    endpoint const& ep,
    reverse_flags flags,
    std::stop_token token,
    std::error_code* ec,
    reverse_resolver_result* result_out)
{
    auto& op = reverse_op_;
    op.reset();
    op.h          = h;
    op.ex         = d;
    op.ec_out     = ec;
    op.result_out = result_out;
    op.impl       = this;
    op.ep         = ep;
    op.flags      = flags;
    op.start(token);

    // Keep io_context alive while resolution is pending
    svc_.work_started();

    // Prevent impl destruction while work is in flight
    reverse_pool_op_.resolver_ = this;
    reverse_pool_op_.ref_      = this->shared_from_this();
    reverse_pool_op_.func_     = &win_resolver::do_reverse_resolve_work;
    if (!svc_.pool().post(&reverse_pool_op_))
    {
        // Pool shut down — complete with cancellation
        reverse_pool_op_.ref_.reset();
        op.cancelled.store(true, std::memory_order_release);
        svc_.work_finished();
        svc_.post(&reverse_op_);
    }
    // completion is always posted to scheduler queue, never inline.
    return std::noop_coroutine();
}

inline void
win_resolver::cancel() noexcept
{
    op_.request_cancel();
    reverse_op_.request_cancel();

    if (op_.cancel_handle)
    {
        ::GetAddrInfoExCancel(&op_.cancel_handle);
    }
}

inline void
win_resolver::do_reverse_resolve_work(pool_work_item* w) noexcept
{
    auto* pw   = static_cast<pool_op*>(w);
    auto* self = pw->resolver_;

    sockaddr_storage ss{};
    int ss_len;

    if (self->reverse_op_.ep.is_v4())
    {
        auto sa = to_sockaddr_in(self->reverse_op_.ep);
        std::memcpy(&ss, &sa, sizeof(sa));
        ss_len = sizeof(sockaddr_in);
    }
    else
    {
        auto sa = to_sockaddr_in6(self->reverse_op_.ep);
        std::memcpy(&ss, &sa, sizeof(sa));
        ss_len = sizeof(sockaddr_in6);
    }

    wchar_t host[NI_MAXHOST];
    wchar_t service[NI_MAXSERV];

    int result = ::GetNameInfoW(
        reinterpret_cast<sockaddr*>(&ss), ss_len, host, NI_MAXHOST, service,
        NI_MAXSERV,
        resolver_detail::flags_to_ni_flags(self->reverse_op_.flags));

    if (!self->reverse_op_.cancelled.load(std::memory_order_acquire))
    {
        if (result == 0)
        {
            self->reverse_op_.stored_host = resolver_detail::from_wide(host);
            self->reverse_op_.stored_service =
                resolver_detail::from_wide(service);
            self->reverse_op_.gai_error = 0;
        }
        else
        {
            self->reverse_op_.gai_error = result;
        }
    }

    self->svc_.work_finished();

    // Move ref to stack before post — post may trigger destroy_impl
    // which erases the last shared_ptr, destroying *self (and *pw)
    auto ref = std::move(pw->ref_);
    self->svc_.post(&self->reverse_op_);
}

// win_resolver_service

inline win_resolver_service::win_resolver_service(
    capy::execution_context& ctx, scheduler& sched)
    : sched_(sched)
    , pool_(ctx.use_service<thread_pool>())
{
}

inline win_resolver_service::~win_resolver_service() {}

inline void
win_resolver_service::shutdown()
{
    std::lock_guard<win_mutex> lock(mutex_);

    // Cancel all resolvers (sets cancelled flag checked by pool threads)
    for (auto* impl = resolver_list_.pop_front(); impl != nullptr;
         impl       = resolver_list_.pop_front())
    {
        impl->cancel();
    }

    // Clear the map which releases shared_ptrs.
    // The thread pool service shuts down separately via
    // execution_context service ordering.
    resolver_ptrs_.clear();
}

inline io_object::implementation*
win_resolver_service::construct()
{
    auto ptr   = std::make_shared<win_resolver>(*this);
    auto* impl = ptr.get();

    {
        std::lock_guard<win_mutex> lock(mutex_);
        resolver_list_.push_back(impl);
        resolver_ptrs_[impl] = std::move(ptr);
    }

    return impl;
}

inline void
win_resolver_service::destroy_impl(win_resolver& impl)
{
    std::lock_guard<win_mutex> lock(mutex_);
    resolver_list_.remove(&impl);
    resolver_ptrs_.erase(&impl);
}

inline void
win_resolver_service::post(overlapped_op* op)
{
    sched_.post(op);
}

inline void
win_resolver_service::work_started() noexcept
{
    sched_.work_started();
}

inline void
win_resolver_service::work_finished() noexcept
{
    sched_.work_finished();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_RESOLVER_SERVICE_HPP
