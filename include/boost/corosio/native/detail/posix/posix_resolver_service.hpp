//
// Copyright (c) 2026 Steve Gerbino
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

namespace boost::corosio::detail {

/** Resolver service for POSIX backends.

    Owns all posix_resolver instances and tracks active worker
    threads for safe shutdown synchronization.
*/
class BOOST_COROSIO_DECL posix_resolver_service final
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    using key_type = posix_resolver_service;

    posix_resolver_service(capy::execution_context&, scheduler& sched)
        : sched_(&sched)
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

    void thread_started() noexcept;
    void thread_finished() noexcept;
    bool is_shutting_down() const noexcept;

private:
    scheduler* sched_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> shutting_down_{false};
    std::size_t active_threads_ = 0;
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

    return resolver_results(std::move(entries));
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
    dispatch_coro(ex, h).resume();
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
// NOLINTNEXTLINE(performance-unnecessary-value-param)
posix_resolver::resolve_op::start(std::stop_token token)
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
    dispatch_coro(ex, h).resume();
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
// NOLINTNEXTLINE(performance-unnecessary-value-param)
posix_resolver::reverse_resolve_op::start(std::stop_token token)
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

    // Track thread for safe shutdown
    svc_.thread_started();

    try
    {
        // Prevent impl destruction while worker thread is running
        auto self = this->shared_from_this();
        std::thread worker([this, self = std::move(self)]() {
            struct addrinfo hints{};
            hints.ai_family   = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = posix_resolver_detail::flags_to_hints(op_.flags);

            struct addrinfo* ai = nullptr;
            int result          = ::getaddrinfo(
                op_.host.empty() ? nullptr : op_.host.c_str(),
                op_.service.empty() ? nullptr : op_.service.c_str(), &hints,
                &ai);

            if (!op_.cancelled.load(std::memory_order_acquire))
            {
                if (result == 0 && ai)
                {
                    op_.stored_results = posix_resolver_detail::convert_results(
                        ai, op_.host, op_.service);
                    op_.gai_error = 0;
                }
                else
                {
                    op_.gai_error = result;
                }
            }

            if (ai)
                ::freeaddrinfo(ai);

            // Always post so the scheduler can properly drain the op
            // during shutdown via destroy().
            svc_.post(&op_);

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
        op_.gai_error = EAI_MEMORY; // Map to "not enough memory"
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

    // Track thread for safe shutdown
    svc_.thread_started();

    try
    {
        // Prevent impl destruction while worker thread is running
        auto self = this->shared_from_this();
        std::thread worker([this, self = std::move(self)]() {
            // Build sockaddr from endpoint
            sockaddr_storage ss{};
            socklen_t ss_len;

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

            char host[NI_MAXHOST];
            char service[NI_MAXSERV];

            int result = ::getnameinfo(
                reinterpret_cast<sockaddr*>(&ss), ss_len, host, sizeof(host),
                service, sizeof(service),
                posix_resolver_detail::flags_to_ni_flags(reverse_op_.flags));

            if (!reverse_op_.cancelled.load(std::memory_order_acquire))
            {
                if (result == 0)
                {
                    reverse_op_.stored_host    = host;
                    reverse_op_.stored_service = service;
                    reverse_op_.gai_error      = 0;
                }
                else
                {
                    reverse_op_.gai_error = result;
                }
            }

            // Always post so the scheduler can properly drain the op
            // during shutdown via destroy().
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
        reverse_op_.gai_error = EAI_MEMORY;
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

// posix_resolver_service implementation

inline void
posix_resolver_service::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Signal threads to not access service after getaddrinfo returns
        shutting_down_.store(true, std::memory_order_release);

        // Cancel all resolvers (sets cancelled flag checked by threads)
        for (auto* impl = resolver_list_.pop_front(); impl != nullptr;
             impl       = resolver_list_.pop_front())
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

inline void
posix_resolver_service::thread_started() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++active_threads_;
}

inline void
posix_resolver_service::thread_finished() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    --active_threads_;
    cv_.notify_one();
}

inline bool
posix_resolver_service::is_shutting_down() const noexcept
{
    return shutting_down_.load(std::memory_order_acquire);
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
