//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_POSIX_RESOLVER_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_POSIX_RESOLVER_SERVICE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/resolver_results.hpp>
#include <boost/capy/ex/any_dispatcher.hpp>
#include <boost/capy/concept/affine_awaitable.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/core/intrusive_list.hpp>

#include <mutex>
#include <stdexcept>

namespace boost {
namespace corosio {
namespace detail {

class posix_resolver_service;
class posix_resolver_impl;

//------------------------------------------------------------------------------

/** Resolver implementation stub for POSIX platforms.

    This is a placeholder implementation that allows compilation on
    POSIX platforms. Operations throw std::logic_error indicating
    the functionality is not yet implemented.

    @note Full POSIX resolver support is planned for a future release.
*/
class posix_resolver_impl
    : public resolver::resolver_impl
    , public capy::intrusive_list<posix_resolver_impl>::node
{
    friend class posix_resolver_service;

public:
    explicit posix_resolver_impl(posix_resolver_service& svc) noexcept
        : svc_(svc)
    {
    }

    void release() override;

    void resolve(
        std::coroutine_handle<>,
        capy::any_dispatcher,
        std::string_view /*host*/,
        std::string_view /*service*/,
        resolve_flags /*flags*/,
        std::stop_token,
        system::error_code*,
        resolver_results*) override
    {
        throw std::logic_error("posix resolver resolve not implemented");
    }

    void cancel() noexcept { /* stub */ }

private:
    posix_resolver_service& svc_;
};

//------------------------------------------------------------------------------

/** POSIX resolver service stub.

    This service provides placeholder implementations for DNS
    resolution on POSIX platforms. Operations throw std::logic_error.

    @note Full POSIX resolver support is planned for a future release.
*/
class posix_resolver_service
    : public capy::execution_context::service
{
public:
    using key_type = posix_resolver_service;

    /** Construct the resolver service.

        @param ctx Reference to the owning execution_context.
    */
    explicit posix_resolver_service(capy::execution_context& /*ctx*/)
    {
    }

    /** Destroy the resolver service. */
    ~posix_resolver_service()
    {
    }

    posix_resolver_service(posix_resolver_service const&) = delete;
    posix_resolver_service& operator=(posix_resolver_service const&) = delete;

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
    posix_resolver_impl& create_impl()
    {
        std::lock_guard lock(mutex_);
        auto* impl = new posix_resolver_impl(*this);
        resolver_list_.push_back(impl);
        return *impl;
    }

    /** Destroy a resolver implementation. */
    void destroy_impl(posix_resolver_impl& impl)
    {
        std::lock_guard lock(mutex_);
        resolver_list_.remove(&impl);
        delete &impl;
    }

private:
    std::mutex mutex_;
    capy::intrusive_list<posix_resolver_impl> resolver_list_;
};

//------------------------------------------------------------------------------

inline void
posix_resolver_impl::
release()
{
    svc_.destroy_impl(*this);
}

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
