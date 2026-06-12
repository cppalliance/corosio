//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_RESOLVER_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_RESOLVER_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/native/detail/posix/posix_resolver.hpp>
#include <boost/corosio/native/detail/reactor/reactor_scheduler.hpp>
#include <boost/corosio/detail/thread_pool.hpp>

#include <unordered_map>

namespace boost::corosio::detail {

/** Resolver service for POSIX backends.

    Owns all posix_resolver instances. Thread lifecycle is managed
    by the thread_pool service.
*/
class BOOST_COROSIO_DECL posix_resolver_service final
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    using key_type = posix_resolver_service;

    posix_resolver_service(capy::execution_context& ctx, scheduler& sched)
        : sched_(&sched)
        , pool_(ctx.use_service<thread_pool>())
    {
    }

    ~posix_resolver_service() override = default;

    posix_resolver_service(posix_resolver_service const&)            = delete;
    posix_resolver_service& operator=(posix_resolver_service const&) = delete;

    io_object::implementation* construct() override;

    void destroy(io_object::implementation* p) override
    {
        auto& impl = static_cast<posix_resolver&>(*p);
        impl.cancel();
        destroy_impl(impl);
    }

    void shutdown() override;
    void destroy_impl(posix_resolver& impl);

    void post(scheduler_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

    /** Return the resolver thread pool. */
    thread_pool& pool() noexcept
    {
        return pool_;
    }

    /** Return true if single-threaded mode is active. */
    bool single_threaded() const noexcept
    {
        return sched_->is_single_threaded();
    }

private:
    scheduler* sched_;
    thread_pool& pool_;
    std::mutex mutex_;
    intrusive_list<posix_resolver> resolver_list_;
    std::unordered_map<posix_resolver*, std::shared_ptr<posix_resolver>>
        resolver_ptrs_;
};

/** Get or create the resolver service for the given context.

    This function is called by the concrete scheduler during initialization
    to create the resolver service with a reference to itself.

    @param ctx Reference to the owning execution_context.
    @param sched Reference to the scheduler for posting completions.
    @return Reference to the resolver service.
*/
posix_resolver_service&
get_resolver_service(capy::execution_context& ctx, scheduler& sched);

// ---------------------------------------------------------------------------
// Inline implementation
// ---------------------------------------------------------------------------

// posix_resolver_detail helpers

inline int
posix_resolver_detail::flags_to_hints(resolve_flags flags)
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

inline int
posix_resolver_detail::flags_to_ni_flags(reverse_flags flags)
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

inline resolver_results
posix_resolver_detail::convert_results(
    struct addrinfo* ai, std::string_view host, std::string_view service)
{
    std::vector<resolver_entry> entries;
    entries.reserve(4); // Most lookups return 1-4 addresses

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

inline std::error_code
posix_resolver_detail::make_gai_error(int gai_err)
{
    // Map GAI errors to appropriate generic error codes
    switch (gai_err)
    {
    case EAI_AGAIN:
        // Temporary failure - try again later
        return std::error_code(
            static_cast<int>(std::errc::resource_unavailable_try_again),
            std::generic_category());

    case EAI_BADFLAGS:
        // Invalid flags
        return std::error_code(
            static_cast<int>(std::errc::invalid_argument),
            std::generic_category());

    case EAI_FAIL:
        // Non-recoverable failure
        return std::error_code(
            static_cast<int>(std::errc::io_error), std::generic_category());

    case EAI_FAMILY:
        // Address family not supported
        return std::error_code(
            static_cast<int>(std::errc::address_family_not_supported),
            std::generic_category());

    case EAI_MEMORY:
        // Memory allocation failure
        return std::error_code(
            static_cast<int>(std::errc::not_enough_memory),
            std::generic_category());

    case EAI_NONAME:
        // Host or service not found
        return std::error_code(
            static_cast<int>(std::errc::no_such_device_or_address),
            std::generic_category());

    case EAI_SERVICE:
        // Service not supported for socket type
        return std::error_code(
            static_cast<int>(std::errc::invalid_argument),
            std::generic_category());

    case EAI_SOCKTYPE:
        // Socket type not supported
        return std::error_code(
            static_cast<int>(std::errc::not_supported),
            std::generic_category());

    case EAI_SYSTEM:
        // System error - use errno
        return std::error_code(errno, std::generic_category());

    default:
        // Unknown error
        return std::error_code(
            static_cast<int>(std::errc::io_error), std::generic_category());
    }
}

// posix_resolver

inline posix_resolver::posix_resolver(posix_resolver_service& svc) noexcept
    : svc_(svc)
{
}

// posix_resolver::resolve_op implementation

inline void
posix_resolver::resolve_op::reset() noexcept
{
    host.clear();
    service.clear();
    flags          = resolve_flags::none;
    stored_results = resolver_results{};
    gai_error      = 0;
    cancelled.store(false, std::memory_order_relaxed);
    stop_cb.reset();
    ec_out = nullptr;
    out    = nullptr;
}

inline void
posix_resolver::resolve_op::operator()()
{
    stop_cb.reset(); // Disconnect stop callback

    bool const was_cancelled = cancelled.load(std::memory_order_acquire);

    if (ec_out)
    {
        if (was_cancelled)
            *ec_out = capy::error::canceled;
        else if (gai_error != 0)
            *ec_out = posix_resolver_detail::make_gai_error(gai_error);
        else
            *ec_out = {}; // Clear on success
    }

    if (out && !was_cancelled && gai_error == 0)
        *out = std::move(stored_results);

    impl->svc_.work_finished();
    cont_op.cont.h = h;
    dispatch_coro(ex, cont_op.cont).resume();
}

inline void
posix_resolver::resolve_op::destroy()
{
    stop_cb.reset();
}

inline void
posix_resolver::resolve_op::request_cancel() noexcept
{
    cancelled.store(true, std::memory_order_release);
}

inline void
posix_resolver::resolve_op::start(std::stop_token const& token)
{
    cancelled.store(false, std::memory_order_release);
    stop_cb.reset();

    if (token.stop_possible())
        stop_cb.emplace(token, canceller{this});
}

// posix_resolver::reverse_resolve_op implementation

inline void
posix_resolver::reverse_resolve_op::reset() noexcept
{
    ep    = endpoint{};
    flags = reverse_flags::none;
    stored_host.clear();
    stored_service.clear();
    gai_error = 0;
    cancelled.store(false, std::memory_order_relaxed);
    stop_cb.reset();
    ec_out     = nullptr;
    result_out = nullptr;
}

inline void
posix_resolver::reverse_resolve_op::operator()()
{
    stop_cb.reset(); // Disconnect stop callback

    bool const was_cancelled = cancelled.load(std::memory_order_acquire);

    if (ec_out)
    {
        if (was_cancelled)
            *ec_out = capy::error::canceled;
        else if (gai_error != 0)
            *ec_out = posix_resolver_detail::make_gai_error(gai_error);
        else
            *ec_out = {}; // Clear on success
    }

    if (result_out && !was_cancelled && gai_error == 0)
    {
        *result_out = reverse_resolver_result(
            ep, std::move(stored_host), std::move(stored_service));
    }

    impl->svc_.work_finished();
    cont_op.cont.h = h;
    dispatch_coro(ex, cont_op.cont).resume();
}

inline void
posix_resolver::reverse_resolve_op::destroy()
{
    stop_cb.reset();
}

inline void
posix_resolver::reverse_resolve_op::request_cancel() noexcept
{
    cancelled.store(true, std::memory_order_release);
}

inline void
posix_resolver::reverse_resolve_op::start(std::stop_token const& token)
{
    cancelled.store(false, std::memory_order_release);
    stop_cb.reset();

    if (token.stop_possible())
        stop_cb.emplace(token, canceller{this});
}

// posix_resolver implementation

inline std::coroutine_handle<>
posix_resolver::resolve(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    std::string_view host,
    std::string_view service,
    resolve_flags flags,
    std::stop_token token,
    std::error_code* ec,
    resolver_results* out)
{
    if (svc_.single_threaded())
    {
        *ec = std::make_error_code(std::errc::operation_not_supported);
        op_.cont_op.cont.h = h;
        return dispatch_coro(ex, op_.cont_op.cont);
    }

    auto& op = op_;
    op.reset();
    op.h       = h;
    op.ex      = ex;
    op.impl    = this;
    op.ec_out  = ec;
    op.out     = out;
    op.host    = host;
    op.service = service;
    op.flags   = flags;
    op.start(token);

    // Keep io_context alive while resolution is pending
    op.ex.on_work_started();

    // Prevent impl destruction while work is in flight
    resolve_pool_op_.resolver_ = this;
    resolve_pool_op_.ref_      = this->shared_from_this();
    resolve_pool_op_.func_     = &posix_resolver::do_resolve_work;
    if (!svc_.pool().post(&resolve_pool_op_))
    {
        // Pool shut down — complete with cancellation
        resolve_pool_op_.ref_.reset();
        op.cancelled.store(true, std::memory_order_release);
        svc_.post(&op_);
    }
    return std::noop_coroutine();
}

inline std::coroutine_handle<>
posix_resolver::reverse_resolve(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    endpoint const& ep,
    reverse_flags flags,
    std::stop_token token,
    std::error_code* ec,
    reverse_resolver_result* result_out)
{
    if (svc_.single_threaded())
    {
        *ec = std::make_error_code(std::errc::operation_not_supported);
        reverse_op_.cont_op.cont.h = h;
        return dispatch_coro(ex, reverse_op_.cont_op.cont);
    }

    auto& op = reverse_op_;
    op.reset();
    op.h          = h;
    op.ex         = ex;
    op.impl       = this;
    op.ec_out     = ec;
    op.result_out = result_out;
    op.ep         = ep;
    op.flags      = flags;
    op.start(token);

    // Keep io_context alive while resolution is pending
    op.ex.on_work_started();

    // Prevent impl destruction while work is in flight
    reverse_pool_op_.resolver_ = this;
    reverse_pool_op_.ref_      = this->shared_from_this();
    reverse_pool_op_.func_     = &posix_resolver::do_reverse_resolve_work;
    if (!svc_.pool().post(&reverse_pool_op_))
    {
        // Pool shut down — complete with cancellation
        reverse_pool_op_.ref_.reset();
        op.cancelled.store(true, std::memory_order_release);
        svc_.post(&reverse_op_);
    }
    return std::noop_coroutine();
}

inline void
posix_resolver::cancel() noexcept
{
    op_.request_cancel();
    reverse_op_.request_cancel();
}

inline void
posix_resolver::do_resolve_work(pool_work_item* w) noexcept
{
    auto* pw   = static_cast<pool_op*>(w);
    auto* self = pw->resolver_;

    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = posix_resolver_detail::flags_to_hints(self->op_.flags);

    struct addrinfo* ai = nullptr;
    int result          = ::getaddrinfo(
        self->op_.host.empty() ? nullptr : self->op_.host.c_str(),
        self->op_.service.empty() ? nullptr : self->op_.service.c_str(), &hints,
        &ai);

    if (!self->op_.cancelled.load(std::memory_order_acquire))
    {
        if (result == 0 && ai)
        {
            self->op_.stored_results = posix_resolver_detail::convert_results(
                ai, self->op_.host, self->op_.service);
            self->op_.gai_error = 0;
        }
        else
        {
            self->op_.gai_error = result;
        }
    }

    if (ai)
        ::freeaddrinfo(ai);

    // Move ref to stack before post — post may trigger destroy_impl
    // which erases the last shared_ptr, destroying *self (and *pw)
    auto ref = std::move(pw->ref_);
    self->svc_.post(&self->op_);
}

inline void
posix_resolver::do_reverse_resolve_work(pool_work_item* w) noexcept
{
    auto* pw   = static_cast<pool_op*>(w);
    auto* self = pw->resolver_;

    sockaddr_storage ss{};
    socklen_t ss_len;

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

    char host[NI_MAXHOST];
    char service[NI_MAXSERV];

    int result = ::getnameinfo(
        reinterpret_cast<sockaddr*>(&ss), ss_len, host, sizeof(host), service,
        sizeof(service),
        posix_resolver_detail::flags_to_ni_flags(self->reverse_op_.flags));

    if (!self->reverse_op_.cancelled.load(std::memory_order_acquire))
    {
        if (result == 0)
        {
            self->reverse_op_.stored_host    = host;
            self->reverse_op_.stored_service = service;
            self->reverse_op_.gai_error      = 0;
        }
        else
        {
            self->reverse_op_.gai_error = result;
        }
    }

    // Move ref to stack before post — post may trigger destroy_impl
    // which erases the last shared_ptr, destroying *self (and *pw)
    auto ref = std::move(pw->ref_);
    self->svc_.post(&self->reverse_op_);
}

// posix_resolver_service implementation

inline void
posix_resolver_service::shutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);

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
posix_resolver_service::construct()
{
    auto ptr   = std::make_shared<posix_resolver>(*this);
    auto* impl = ptr.get();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        resolver_list_.push_back(impl);
        resolver_ptrs_[impl] = std::move(ptr);
    }

    return impl;
}

inline void
posix_resolver_service::destroy_impl(posix_resolver& impl)
{
    std::lock_guard<std::mutex> lock(mutex_);
    resolver_list_.remove(&impl);
    resolver_ptrs_.erase(&impl);
}

inline void
posix_resolver_service::post(scheduler_op* op)
{
    sched_->post(op);
}

inline void
posix_resolver_service::work_started() noexcept
{
    sched_->work_started();
}

inline void
posix_resolver_service::work_finished() noexcept
{
    sched_->work_finished();
}

// Free function to get/create the resolver service

inline posix_resolver_service&
get_resolver_service(capy::execution_context& ctx, scheduler& sched)
{
    return ctx.make_service<posix_resolver_service>(sched);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_RESOLVER_SERVICE_HPP
