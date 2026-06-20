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

#ifndef BOOST_COROSIO_RESOLVER_HPP
#define BOOST_COROSIO_RESOLVER_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/corosio/resolver_results.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/concept/executor.hpp>

#include <system_error>

#include <cassert>
#include <concepts>
#include <coroutine>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>

namespace boost::corosio {

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
inline resolve_flags
operator|(resolve_flags a, resolve_flags b) noexcept
{
    return static_cast<resolve_flags>(
        static_cast<unsigned int>(a) | static_cast<unsigned int>(b));
}

/** Combine two resolve_flags. */
inline resolve_flags&
operator|=(resolve_flags& a, resolve_flags b) noexcept
{
    a = a | b;
    return a;
}

/** Intersect two resolve_flags. */
inline resolve_flags
operator&(resolve_flags a, resolve_flags b) noexcept
{
    return static_cast<resolve_flags>(
        static_cast<unsigned int>(a) & static_cast<unsigned int>(b));
}

/** Intersect two resolve_flags. */
inline resolve_flags&
operator&=(resolve_flags& a, resolve_flags b) noexcept
{
    a = a & b;
    return a;
}

/** Bitmask flags for reverse resolver queries.

    These flags correspond to the flags parameter of getnameinfo.
*/
enum class reverse_flags : unsigned int
{
    /// No flags.
    none = 0,

    /// Return the numeric form of the hostname instead of its name.
    numeric_host = 0x01,

    /// Return the numeric form of the service name instead of its name.
    numeric_service = 0x02,

    /// Return an error if the hostname cannot be resolved.
    name_required = 0x04,

    /// Lookup for datagram (UDP) service instead of stream (TCP).
    datagram_service = 0x08
};

/** Combine two reverse_flags. */
inline reverse_flags
operator|(reverse_flags a, reverse_flags b) noexcept
{
    return static_cast<reverse_flags>(
        static_cast<unsigned int>(a) | static_cast<unsigned int>(b));
}

/** Combine two reverse_flags. */
inline reverse_flags&
operator|=(reverse_flags& a, reverse_flags b) noexcept
{
    a = a | b;
    return a;
}

/** Intersect two reverse_flags. */
inline reverse_flags
operator&(reverse_flags a, reverse_flags b) noexcept
{
    return static_cast<reverse_flags>(
        static_cast<unsigned int>(a) & static_cast<unsigned int>(b));
}

/** Intersect two reverse_flags. */
inline reverse_flags&
operator&=(reverse_flags& a, reverse_flags b) noexcept
{
    a = a & b;
    return a;
}

/** An asynchronous DNS resolver for coroutine I/O.

    This class provides asynchronous DNS resolution operations that return
    awaitable types. Each operation participates in the affine awaitable
    protocol, ensuring coroutines resume on the correct executor.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. A resolver must not have concurrent resolve
    operations.

    @par Semantics
    Wraps platform DNS resolution (getaddrinfo/getnameinfo).
    Operations dispatch to OS resolver APIs via the io_context
    thread pool.

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

    // Or, to convert errors into exceptions:
    auto [ec2, results2] = co_await r.resolve("www.example.com", "https");
    if (ec2)
        throw std::system_error(ec2);
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
        mutable std::error_code ec_;
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

        capy::io_result<resolver_results> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(std::errc::operation_canceled), {}};
            return {ec_, std::move(results_)};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            return r_.get().resolve(
                h, env->executor, host_, service_, flags_, token_, &ec_,
                &results_);
        }
    };

    struct reverse_resolve_awaitable
    {
        resolver& r_;
        endpoint ep_;
        reverse_flags flags_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable reverse_resolver_result result_;

        reverse_resolve_awaitable(
            resolver& r, endpoint const& ep, reverse_flags flags) noexcept
            : r_(r)
            , ep_(ep)
            , flags_(flags)
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        capy::io_result<reverse_resolver_result> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(std::errc::operation_canceled), {}};
            return {ec_, std::move(result_)};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            return r_.get().reverse_resolve(
                h, env->executor, ep_, flags_, token_, &ec_, &result_);
        }
    };

public:
    /** Destructor.

        Cancels any pending operations.
    */
    ~resolver() override;

    /** Construct a resolver from an execution context.

        @param ctx The execution context that will own this resolver.
    */
    explicit resolver(capy::execution_context& ctx);

    /** Construct a resolver from an executor.

        The resolver is associated with the executor's context.

        @param ex The executor whose context will own the resolver.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, resolver>) &&
        capy::Executor<Ex>
    explicit resolver(Ex const& ex) : resolver(ex.context())
    {
    }

    /** Move constructor.

        Transfers ownership of the resolver resources. After the move,
        @p other is in a moved-from state and may only be destroyed or
        assigned to.

        @param other The resolver to move from.

        @pre No awaitables returned by @p other's `resolve` methods
            exist.
        @pre The execution context associated with @p other must
            outlive this resolver.
    */
    resolver(resolver&& other) noexcept : io_object(std::move(other)) {}

    /** Move assignment operator.

        Destroys the current implementation and transfers ownership
        from @p other. After the move, @p other is in a moved-from
        state and may only be destroyed or assigned to.

        @param other The resolver to move from.

        @pre No awaitables returned by either `*this` or @p other's
            `resolve` methods exist.
        @pre The execution context associated with @p other must
            outlive this resolver.

        @return Reference to this resolver.
    */
    resolver& operator=(resolver&& other) noexcept
    {
        if (this != &other)
            h_ = std::move(other.h_);
        return *this;
    }

    resolver(resolver const&)            = delete;
    resolver& operator=(resolver const&) = delete;

    /** Initiate an asynchronous resolve operation.

        Resolves the host and service names into a list of endpoints.

        This resolver must outlive the returned awaitable.

        @param host A string identifying a location. May be a descriptive
            name or a numeric address string.

        @param service A string identifying the requested service. This may
            be a descriptive name or a numeric string corresponding to a
            port number.

        @return An awaitable that completes with `io_result<resolver_results>`.

        @note `resolver_results` is an alias for `std::vector<resolver_entry>`.
            Copying it deep-copies every entry (each owns two `std::string`s);
            move it (`std::move(results)`) or pass iterators when handing it to
            a by-value sink such as @ref connect.

        @par Example
        @code
        auto [ec, results] = co_await r.resolve("www.example.com", "https");
        @endcode
    */
    auto resolve(std::string_view host, std::string_view service)
    {
        return resolve_awaitable(*this, host, service, resolve_flags::none);
    }

    /** Initiate an asynchronous resolve operation with flags.

        Resolves the host and service names into a list of endpoints.

        This resolver must outlive the returned awaitable.

        @param host A string identifying a location.

        @param service A string identifying the requested service.

        @param flags Flags controlling resolution behavior.

        @return An awaitable that completes with `io_result<resolver_results>`.
    */
    auto resolve(
        std::string_view host, std::string_view service, resolve_flags flags)
    {
        return resolve_awaitable(*this, host, service, flags);
    }

    /** Initiate an asynchronous reverse resolve operation.

        Resolves an endpoint into a hostname and service name using
        reverse DNS lookup (PTR record query).

        This resolver must outlive the returned awaitable.

        @param ep The endpoint to resolve.

        @return An awaitable that completes with
            `io_result<reverse_resolver_result>`.

        @par Example
        @code
        endpoint ep(ipv4_address({127, 0, 0, 1}), 80);
        auto [ec, result] = co_await r.resolve(ep);
        if (!ec)
            std::cout << result.host_name() << ":" << result.service_name();
        @endcode
    */
    auto resolve(endpoint const& ep)
    {
        return reverse_resolve_awaitable(*this, ep, reverse_flags::none);
    }

    /** Initiate an asynchronous reverse resolve operation with flags.

        Resolves an endpoint into a hostname and service name using
        reverse DNS lookup (PTR record query).

        This resolver must outlive the returned awaitable.

        @param ep The endpoint to resolve.

        @param flags Flags controlling resolution behavior. See reverse_flags.

        @return An awaitable that completes with
            `io_result<reverse_resolver_result>`.
    */
    auto resolve(endpoint const& ep, reverse_flags flags)
    {
        return reverse_resolve_awaitable(*this, ep, flags);
    }

    /** Cancel any pending asynchronous operations.

        All outstanding operations complete with `errc::operation_canceled`.
        Check `ec == cond::canceled` for portable comparison.
    */
    void cancel();

public:
    /** Backend interface for DNS resolution operations.

        Platform backends derive from this to implement forward and
        reverse DNS resolution via getaddrinfo/getnameinfo.
    */
    struct implementation : io_object::implementation
    {
        /// Initiate an asynchronous forward DNS resolution.
        virtual std::coroutine_handle<> resolve(
            std::coroutine_handle<>,
            capy::executor_ref,
            std::string_view host,
            std::string_view service,
            resolve_flags flags,
            std::stop_token,
            std::error_code*,
            resolver_results*) = 0;

        /// Initiate an asynchronous reverse DNS resolution.
        virtual std::coroutine_handle<> reverse_resolve(
            std::coroutine_handle<>,
            capy::executor_ref,
            endpoint const& ep,
            reverse_flags flags,
            std::stop_token,
            std::error_code*,
            reverse_resolver_result*) = 0;

        /// Cancel pending resolve operations.
        virtual void cancel() noexcept = 0;
    };

protected:
    explicit resolver(handle h) noexcept : io_object(std::move(h)) {}

private:
    inline implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif
