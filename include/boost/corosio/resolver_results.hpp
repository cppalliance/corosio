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

#if !defined(BOOST_COROSIO_SOURCE) && defined(BOOST_COROSIO_USE_MODULES)
import boost.corosio;
#else

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/endpoint.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace boost {
namespace corosio {

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
        static std::vector<resolver_entry> empty;
        return empty.begin();
    }

    /** Get an iterator past the last entry. */
    const_iterator
    end() const noexcept
    {
        if (entries_)
            return entries_->end();
        static std::vector<resolver_entry> empty;
        return empty.end();
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

} // namespace corosio
} // namespace boost

#endif
#endif
