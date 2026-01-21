//
// Copyright (c) 2026 Cinar Gursoy
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_KQUEUE_RESOLVER_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_KQUEUE_RESOLVER_SERVICE_HPP

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_KQUEUE)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/resolver_results.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/concept/io_awaitable.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include "src/detail/intrusive.hpp"

#include <mutex>
#include <stdexcept>

namespace boost {
namespace corosio {
namespace detail {

class kqueue_resolver_service;
class kqueue_resolver_impl;

//------------------------------------------------------------------------------

/** Resolver implementation stub for macOS/BSD.

    This is a placeholder implementation that allows compilation on
    macOS/BSD. Operations throw std::logic_error indicating the
    functionality is not yet implemented.

    @note Full resolver support is planned for a future release.
*/
class kqueue_resolver_impl
    : public resolver::resolver_impl
    , public intrusive_list<kqueue_resolver_impl>::node
{
    friend class kqueue_resolver_service;

public:
    explicit kqueue_resolver_impl(kqueue_resolver_service& svc) noexcept
        : svc_(svc)
    {
    }

    void release() override;

    void resolve(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::string_view /*host*/,
        std::string_view /*service*/,
        resolve_flags /*flags*/,
        capy::stop_token,
        system::error_code*,
        resolver_results*) override
    {
        throw std::logic_error("kqueue resolver resolve not implemented");
    }

    void cancel() noexcept { /* stub */ }

private:
    kqueue_resolver_service& svc_;
};

//------------------------------------------------------------------------------

/** macOS/BSD resolver service stub.

    This service provides placeholder implementations for DNS
    resolution on macOS/BSD. Operations throw std::logic_error.

    @note Full resolver support is planned for a future release.
*/
class kqueue_resolver_service
    : public capy::execution_context::service
{
public:
    using key_type = kqueue_resolver_service;

    /** Construct the resolver service.

        @param ctx Reference to the owning execution_context.
    */
    explicit kqueue_resolver_service(capy::execution_context& /*ctx*/)
    {
    }

    /** Destroy the resolver service. */
    ~kqueue_resolver_service()
    {
    }

    kqueue_resolver_service(kqueue_resolver_service const&) = delete;
    kqueue_resolver_service& operator=(kqueue_resolver_service const&) = delete;

    /** Shut down the service. */
    void shutdown() override
    {
        std::lock_guard lock(mutex_);

        // Release all resolvers
        while (auto* impl = resolver_list_.pop_front())
        {
            delete impl;
        }
    }

    /** Create a new resolver implementation. */
    kqueue_resolver_impl& create_impl()
    {
        std::lock_guard lock(mutex_);
        auto* impl = new kqueue_resolver_impl(*this);
        resolver_list_.push_back(impl);
        return *impl;
    }

    /** Destroy a resolver implementation. */
    void destroy_impl(kqueue_resolver_impl& impl)
    {
        std::lock_guard lock(mutex_);
        resolver_list_.remove(&impl);
        delete &impl;
    }

private:
    std::mutex mutex_;
    intrusive_list<kqueue_resolver_impl> resolver_list_;
};

//------------------------------------------------------------------------------

inline void
kqueue_resolver_impl::
release()
{
    svc_.destroy_impl(*this);
}

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // BOOST_COROSIO_BACKEND_KQUEUE

#endif // BOOST_COROSIO_DETAIL_KQUEUE_RESOLVER_SERVICE_HPP
