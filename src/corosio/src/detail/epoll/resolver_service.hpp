//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_EPOLL_RESOLVER_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_EPOLL_RESOLVER_SERVICE_HPP

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_EPOLL)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/resolver_results.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/io_awaitable.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include "src/detail/intrusive.hpp"

#include <mutex>
#include <stdexcept>

namespace boost {
namespace corosio {
namespace detail {

class epoll_resolver_service;
class epoll_resolver_impl;

//------------------------------------------------------------------------------

/** Resolver implementation stub for Linux.

    This is a placeholder implementation that allows compilation on
    Linux. Operations throw std::logic_error indicating the
    functionality is not yet implemented.

    @note Full resolver support is planned for a future release.
*/
class epoll_resolver_impl
    : public resolver::resolver_impl
    , public intrusive_list<epoll_resolver_impl>::node
{
    friend class epoll_resolver_service;

public:
    explicit epoll_resolver_impl(epoll_resolver_service& svc) noexcept
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
        throw std::logic_error("epoll resolver resolve not implemented");
    }

    void cancel() noexcept { /* stub */ }

private:
    epoll_resolver_service& svc_;
};

//------------------------------------------------------------------------------

/** Linux resolver service stub.

    This service provides placeholder implementations for DNS
    resolution on Linux. Operations throw std::logic_error.

    @note Full resolver support is planned for a future release.
*/
class epoll_resolver_service
    : public capy::execution_context::service
{
public:
    using key_type = epoll_resolver_service;

    /** Construct the resolver service.

        @param ctx Reference to the owning execution_context.
    */
    explicit epoll_resolver_service(capy::execution_context& /*ctx*/)
    {
    }

    /** Destroy the resolver service. */
    ~epoll_resolver_service()
    {
    }

    epoll_resolver_service(epoll_resolver_service const&) = delete;
    epoll_resolver_service& operator=(epoll_resolver_service const&) = delete;

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
    epoll_resolver_impl& create_impl()
    {
        std::lock_guard lock(mutex_);
        auto* impl = new epoll_resolver_impl(*this);
        resolver_list_.push_back(impl);
        return *impl;
    }

    /** Destroy a resolver implementation. */
    void destroy_impl(epoll_resolver_impl& impl)
    {
        std::lock_guard lock(mutex_);
        resolver_list_.remove(&impl);
        delete &impl;
    }

private:
    std::mutex mutex_;
    intrusive_list<epoll_resolver_impl> resolver_list_;
};

//------------------------------------------------------------------------------

inline void
epoll_resolver_impl::
release()
{
    svc_.destroy_impl(*this);
}

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // BOOST_COROSIO_BACKEND_EPOLL

#endif // BOOST_COROSIO_DETAIL_EPOLL_RESOLVER_SERVICE_HPP
