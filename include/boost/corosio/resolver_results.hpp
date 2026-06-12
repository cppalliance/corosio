//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_RESOLVER_RESULTS_HPP
#define BOOST_COROSIO_RESOLVER_RESULTS_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/endpoint.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace boost::corosio {

/** A single entry produced by a resolver.

    This class represents one resolved endpoint along with
    the host and service names used in the query.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe.
*/
class resolver_entry
{
    endpoint ep_;
    std::string host_name_;
    std::string service_name_;

public:
    /// Construct a default empty entry.
    resolver_entry() = default;

    /** Construct with endpoint, host name, and service name.

        @param ep The resolved endpoint.
        @param host The host name from the query.
        @param service The service name from the query.
    */
    resolver_entry(endpoint ep, std::string_view host, std::string_view service)
        : ep_(ep)
        , host_name_(host)
        , service_name_(service)
    {
    }

    /// Return the resolved endpoint.
    endpoint get_endpoint() const noexcept
    {
        return ep_;
    }

    /// Convert to endpoint.
    operator endpoint() const noexcept
    {
        return ep_;
    }

    /// Return the host name from the query.
    std::string const& host_name() const noexcept
    {
        return host_name_;
    }

    /// Return the service name from the query.
    std::string const& service_name() const noexcept
    {
        return service_name_;
    }
};

/** A range of entries produced by a resolver.

    This is an alias for `std::vector<resolver_entry>`: a contiguous,
    owning range of the endpoints resolved by a query. It supports the
    full `std::vector` interface (iteration, `size()`, `empty()`, etc.).

    @note Copying a `resolver_results` deep-copies every entry, and each
    entry owns two `std::string`s (the host and service names). When you
    want to hand a result to a sink that takes the range by value — such
    as `corosio::connect` — pass an rvalue (`std::move(results)`) or use
    the iterator-based `connect` overloads to avoid the copy.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe.
*/
using resolver_results = std::vector<resolver_entry>;

/** The result of a reverse DNS resolution.

    This class holds the result of resolving an endpoint
    into a hostname and service name.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe.
*/
class reverse_resolver_result
{
    corosio::endpoint ep_;
    std::string host_;
    std::string service_;

public:
    /// Construct a default empty result.
    reverse_resolver_result() = default;

    /** Construct with endpoint, host name, and service name.

        @param ep The endpoint that was resolved.
        @param host The resolved host name.
        @param service The resolved service name.
    */
    reverse_resolver_result(
        corosio::endpoint ep, std::string host, std::string service)
        : ep_(ep)
        , host_(std::move(host))
        , service_(std::move(service))
    {
    }

    /// Return the endpoint that was resolved.
    corosio::endpoint endpoint() const noexcept
    {
        return ep_;
    }

    /// Return the resolved host name.
    std::string const& host_name() const noexcept
    {
        return host_;
    }

    /// Return the resolved service name.
    std::string const& service_name() const noexcept
    {
        return service_;
    }
};

} // namespace boost::corosio

#endif
