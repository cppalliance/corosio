//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_RESOLVER_HPP
#define BOOST_COROSIO_RESOLVER_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/io_object.hpp>
#include <boost/corosio/io_result.hpp>
#include <boost/corosio/resolver_results.hpp>
#include <boost/capy/ex/any_dispatcher.hpp>
#include <boost/capy/concept/affine_awaitable.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/concept/executor.hpp>

#include <boost/system/error_code.hpp>

#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>

namespace boost {
namespace corosio {

/** Bitmask flags for resolver queries.

    These flags correspond to the hints parameter of getaddrinfo.
*/
enum class resolve_flags : unsigned int
{
    /// No flags.
    none = 0,

    /// Indicate that returned endpoint is intended for use as a locally
    /// bound socket endpoint.
    passive = 0x01,

    /// Host name should be treated as a numeric string defining an IPv4
    /// or IPv6 address and no name resolution should be attempted.
    numeric_host = 0x04,

    /// Service name should be treated as a numeric string defining a port
    /// number and no name resolution should be attempted.
    numeric_service = 0x08,

    /// Only return IPv4 addresses if a non-loopback IPv4 address is
    /// configured for the system. Only return IPv6 addresses if a
    /// non-loopback IPv6 address is configured for the system.
    address_configured = 0x20,

    /// If the query protocol family is specified as IPv6, return
    /// IPv4-mapped IPv6 addresses on finding no IPv6 addresses.
    v4_mapped = 0x800,

    /// If used with v4_mapped, return all matching IPv6 and IPv4 addresses.
    all_matching = 0x100
};

/** Combine two resolve_flags. */
inline
resolve_flags
operator|(resolve_flags a, resolve_flags b) noexcept
{
    return static_cast<resolve_flags>(
        static_cast<unsigned int>(a) |
        static_cast<unsigned int>(b));
}

/** Combine two resolve_flags. */
inline
resolve_flags&
operator|=(resolve_flags& a, resolve_flags b) noexcept
{
    a = a | b;
    return a;
}

/** Intersect two resolve_flags. */
inline
resolve_flags
operator&(resolve_flags a, resolve_flags b) noexcept
{
    return static_cast<resolve_flags>(
        static_cast<unsigned int>(a) &
        static_cast<unsigned int>(b));
}

/** Intersect two resolve_flags. */
inline
resolve_flags&
operator&=(resolve_flags& a, resolve_flags b) noexcept
{
    a = a & b;
    return a;
}

//------------------------------------------------------------------------------

/** An asynchronous DNS resolver for coroutine I/O.

    This class provides asynchronous DNS resolution operations that return
    awaitable types. Each operation participates in the affine awaitable
    protocol, ensuring coroutines resume on the correct executor.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. A resolver must not have concurrent resolve
    operations.

    @par Example
    @code
    io_context ioc;
    resolver r(ioc);

    // Using structured bindings
    auto [ec, results] = co_await r.resolve("www.example.com", "https");
    if (ec)
        co_return;

    for (auto const& entry : results)
        std::cout << entry.get_endpoint().port() << std::endl;

    // Or using exceptions
    auto results = (co_await r.resolve("www.example.com", "https")).value();
    @endcode
*/
class BOOST_COROSIO_DECL resolver : public io_object
{
    struct resolve_awaitable
    {
        resolver& r_;
        std::string host_;
        std::string service_;
        resolve_flags flags_;
        std::stop_token token_;
        mutable system::error_code ec_;
        mutable resolver_results results_;

        resolve_awaitable(
            resolver& r,
            std::string_view host,
            std::string_view service,
            resolve_flags flags) noexcept
            : r_(r)
            , host_(host)
            , service_(service)
            , flags_(flags)
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        io_result<resolver_results> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(system::errc::operation_canceled), {}};
            return {ec_, std::move(results_)};
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d) -> std::coroutine_handle<>
        {
            r_.get().resolve(h, d, host_, service_, flags_, token_, &ec_, &results_);
            return std::noop_coroutine();
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d,
            std::stop_token token) -> std::coroutine_handle<>
        {
            token_ = std::move(token);
            r_.get().resolve(h, d, host_, service_, flags_, token_, &ec_, &results_);
            return std::noop_coroutine();
        }
    };

public:
    /** Destructor.

        Cancels any pending operations.
    */
    ~resolver();

    /** Construct a resolver from an execution context.

        @param ctx The execution context that will own this resolver.
    */
    explicit resolver(capy::execution_context& ctx);

    /** Construct a resolver from an executor.

        The resolver is associated with the executor's context.

        @param ex The executor whose context will own the resolver.
    */
    template<class Executor>
        requires (!std::same_as<std::remove_cvref_t<Executor>, resolver>) &&
                 capy::executor<Executor>
    explicit resolver(Executor const& ex)
        : resolver(ex.context())
    {
    }

    /** Move constructor.

        Transfers ownership of the resolver resources.

        @param other The resolver to move from.
    */
    resolver(resolver&& other) noexcept
        : io_object(other.context())
    {
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }

    /** Move assignment operator.

        Cancels any existing operations and transfers ownership.
        The source and destination must share the same execution context.

        @param other The resolver to move from.

        @return Reference to this resolver.

        @throws std::logic_error if the resolvers have different
            execution contexts.
    */
    resolver& operator=(resolver&& other)
    {
        if (this != &other)
        {
            if (ctx_ != other.ctx_)
                detail::throw_logic_error(
                    "cannot move resolver across execution contexts");
            cancel();
            impl_ = other.impl_;
            other.impl_ = nullptr;
        }
        return *this;
    }

    resolver(resolver const&) = delete;
    resolver& operator=(resolver const&) = delete;

    /** Initiate an asynchronous resolve operation.

        Resolves the host and service names into a list of endpoints.

        @param host A string identifying a location. May be a descriptive
            name or a numeric address string.

        @param service A string identifying the requested service. This may
            be a descriptive name or a numeric string corresponding to a
            port number.

        @return An awaitable that completes with `io_result<resolver_results>`.

        @par Example
        @code
        auto [ec, results] = co_await r.resolve("www.example.com", "https");
        @endcode
    */
    auto resolve(
        std::string_view host,
        std::string_view service)
    {
        return resolve_awaitable(*this, host, service, resolve_flags::none);
    }

    /** Initiate an asynchronous resolve operation with flags.

        Resolves the host and service names into a list of endpoints.

        @param host A string identifying a location.

        @param service A string identifying the requested service.

        @param flags Flags controlling resolution behavior.

        @return An awaitable that completes with `io_result<resolver_results>`.
    */
    auto resolve(
        std::string_view host,
        std::string_view service,
        resolve_flags flags)
    {
        return resolve_awaitable(*this, host, service, flags);
    }

    /** Cancel any pending asynchronous operations.

        All outstanding operations complete with `errc::operation_canceled`.
    */
    void cancel();

public:
    struct resolver_impl : io_object_impl
    {
        virtual void resolve(
            std::coroutine_handle<>,
            capy::any_dispatcher,
            std::string_view host,
            std::string_view service,
            resolve_flags flags,
            std::stop_token,
            system::error_code*,
            resolver_results*) = 0;
    };

private:
    inline resolver_impl& get() const noexcept
    {
        return *static_cast<resolver_impl*>(impl_);
    }
};

} // namespace corosio
} // namespace boost

#endif
