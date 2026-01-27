//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
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

#include <cstddef>
#include <memory>
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
    /** Default constructor. */
    resolver_entry() = default;

    /** Construct with endpoint, host name, and service name.

        @param ep The resolved endpoint.
        @param host The host name from the query.
        @param service The service name from the query.
    */
    resolver_entry(
        endpoint ep,
        std::string_view host,
        std::string_view service)
        : ep_(ep)
        , host_name_(host)
        , service_name_(service)
    {
    }

    /** Get the endpoint. */
    endpoint
    get_endpoint() const noexcept
    {
        return ep_;
    }

    /** Implicit conversion to endpoint. */
    operator endpoint() const noexcept
    {
        return ep_;
    }

    /** Get the host name from the query. */
    std::string const&
    host_name() const noexcept
    {
        return host_name_;
    }

    /** Get the service name from the query. */
    std::string const&
    service_name() const noexcept
    {
        return service_name_;
    }
};

//------------------------------------------------------------------------------

/** A range of entries produced by a resolver.

    This class holds the results of a DNS resolution query.
    It provides a range interface for iterating over the
    resolved endpoints.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe (immutable after construction).
*/
class resolver_results
{
public:
    using value_type = resolver_entry;
    using const_reference = value_type const&;
    using reference = const_reference;
    using const_iterator = std::vector<resolver_entry>::const_iterator;
    using iterator = const_iterator;
    using difference_type = std::ptrdiff_t;
    using size_type = std::size_t;

private:
    std::shared_ptr<std::vector<resolver_entry>> entries_;

public:
    /** Default constructor creates an empty range. */
    resolver_results() = default;

    /** Construct from a vector of entries.

        @param entries The resolved entries.
    */
    explicit
    resolver_results(std::vector<resolver_entry> entries)
        : entries_(std::make_shared<std::vector<resolver_entry>>(
            std::move(entries)))
    {
    }

    /** Get the number of entries. */
    size_type
    size() const noexcept
    {
        return entries_ ? entries_->size() : 0;
    }

    /** Check if the results are empty. */
    bool
    empty() const noexcept
    {
        return !entries_ || entries_->empty();
    }

    /** Get an iterator to the first entry. */
    const_iterator
    begin() const noexcept
    {
        if (entries_)
            return entries_->begin();
        return std::vector<resolver_entry>::const_iterator();
    }

    /** Get an iterator past the last entry. */
    const_iterator
    end() const noexcept
    {
        if (entries_)
            return entries_->end();
        return std::vector<resolver_entry>::const_iterator();
    }

    /** Get an iterator to the first entry. */
    const_iterator
    cbegin() const noexcept
    {
        return begin();
    }

    /** Get an iterator past the last entry. */
    const_iterator
    cend() const noexcept
    {
        return end();
    }

    /** Swap with another results object. */
    void
    swap(resolver_results& other) noexcept
    {
        entries_.swap(other.entries_);
    }

    /** Test for equality. */
    friend
    bool
    operator==(
        resolver_results const& a,
        resolver_results const& b) noexcept
    {
        return a.entries_ == b.entries_;
    }

    /** Test for inequality. */
    friend
    bool
    operator!=(
        resolver_results const& a,
        resolver_results const& b) noexcept
    {
        return !(a == b);
    }
};

//------------------------------------------------------------------------------

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
    /** Default constructor. */
    reverse_resolver_result() = default;

    /** Construct with endpoint, host name, and service name.

        @param ep The endpoint that was resolved.
        @param host The resolved host name.
        @param service The resolved service name.
    */
    reverse_resolver_result(
        corosio::endpoint ep,
        std::string host,
        std::string service)
        : ep_(ep)
        , host_(std::move(host))
        , service_(std::move(service))
    {
    }

    /** Get the endpoint that was resolved. */
    corosio::endpoint
    endpoint() const noexcept
    {
        return ep_;
    }

    /** Get the resolved host name. */
    std::string const&
    host_name() const noexcept
    {
        return host_;
    }

    /** Get the resolved service name. */
    std::string const&
    service_name() const noexcept
    {
        return service_;
    }
};

} // namespace boost::corosio

#endif
