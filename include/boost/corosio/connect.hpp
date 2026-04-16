//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_CONNECT_HPP
#define BOOST_COROSIO_CONNECT_HPP

#include <boost/corosio/detail/config.hpp>

#include <boost/capy/cond.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/task.hpp>

#include <concepts>
#include <iterator>
#include <ranges>
#include <system_error>
#include <utility>

/*
  Range-based composed connect operation.

  These free functions try each endpoint in a range (or iterator pair)
  in order, returning on the first successful connect. Between attempts
  the socket is closed so that the next attempt can auto-open with the
  correct address family (e.g. going from IPv4 to IPv6 candidates).

  The iteration semantics follow Boost.Asio's range/iterator async_connect:
  on success, the successful endpoint (or its iterator) is returned; on
  all-fail, the last attempt's error code is returned; on an empty range
  (or when a connect_condition rejects every candidate),
  std::errc::no_such_device_or_address is returned, matching the error
  the resolver uses for "no results" in posix_resolver_service.

  The operation is a plain coroutine; cancellation is propagated to the
  inner per-endpoint connect via the affine awaitable protocol on io_env.
*/

namespace boost::corosio {

namespace detail {

/* Always-true connect condition used by the overloads that take no
   user-supplied predicate. Kept at namespace-detail scope so it has a
   stable linkage name across translation units. */
struct default_connect_condition
{
    template<class Endpoint>
    bool operator()(std::error_code const&, Endpoint const&) const noexcept
    {
        return true;
    }
};

} // namespace detail

/* Forward declarations so the non-condition overloads can delegate
   to the condition overloads via qualified lookup (qualified calls
   bind to the overload set visible at definition, not instantiation). */

template<class Socket, std::ranges::input_range Range, class ConnectCondition>
    requires std::convertible_to<
                 std::ranges::range_reference_t<Range>,
                 typename Socket::endpoint_type> &&
    std::predicate<
                 ConnectCondition&,
                 std::error_code const&,
                 typename Socket::endpoint_type const&>
capy::task<capy::io_result<typename Socket::endpoint_type>>
connect(Socket& s, Range endpoints, ConnectCondition cond);

template<class Socket, std::input_iterator Iter, class ConnectCondition>
    requires std::convertible_to<
                 std::iter_reference_t<Iter>,
                 typename Socket::endpoint_type> &&
    std::predicate<
                 ConnectCondition&,
                 std::error_code const&,
                 typename Socket::endpoint_type const&>
capy::task<capy::io_result<Iter>>
connect(Socket& s, Iter begin, Iter end, ConnectCondition cond);

/** Asynchronously connect a socket by trying each endpoint in a range.

    Each candidate is tried in order. Before each attempt the socket is
    closed (so the next `connect` auto-opens with the candidate's
    address family). On first successful connect, the operation
    completes with the connected endpoint.

    @par Cancellation
    Supports cancellation via the affine awaitable protocol. If a
    per-endpoint connect completes with `capy::cond::canceled` the
    operation completes immediately with that error and does not try
    further endpoints.

    @param s The socket to connect. Must have a `connect(endpoint)`
        member returning an awaitable, plus `close()` and `is_open()`.
        If the socket is already open, it will be closed before the
        first attempt.
    @param endpoints A range of candidate endpoints. Taken by value
        so temporaries (e.g. `resolver_results` returned from
        `resolver::resolve`) remain alive for the coroutine's lifetime.

    @return An awaitable completing with
        `capy::io_result<typename Socket::endpoint_type>`:
        - on success: default error_code and the connected endpoint;
        - on failure of all attempts: the error from the last attempt
          and a default-constructed endpoint;
        - on empty range: `std::errc::no_such_device_or_address` and a
          default-constructed endpoint.

    @note The socket is closed and re-opened before each attempt, so
        any socket options set by the caller (e.g. `no_delay`,
        `reuse_address`) are lost. Apply options after this operation
        completes.

    @throws std::system_error if auto-opening the socket fails during
        an attempt (inherits the contract of `Socket::connect`).

    @par Example
    @code
    resolver r(ioc);
    auto [rec, results] = co_await r.resolve("www.boost.org", "80");
    if (rec) co_return;
    tcp_socket s(ioc);
    auto [cec, ep] = co_await corosio::connect(s, results);
    @endcode
*/
template<class Socket, std::ranges::input_range Range>
    requires std::convertible_to<
        std::ranges::range_reference_t<Range>,
        typename Socket::endpoint_type>
capy::task<capy::io_result<typename Socket::endpoint_type>>
connect(Socket& s, Range endpoints)
{
    return corosio::connect(
        s, std::move(endpoints), detail::default_connect_condition{});
}

/** Asynchronously connect a socket by trying each endpoint in a range,
    filtered by a user-supplied condition.

    For each candidate the condition is invoked as
    `cond(last_ec, ep)` where `last_ec` is the error from the most
    recent attempt (default-constructed before the first attempt). If
    the condition returns `false` the candidate is skipped; otherwise a
    connect is attempted.

    @param s The socket to connect. See the non-condition overload for
        requirements.
    @param endpoints A range of candidate endpoints.
    @param cond A predicate invocable with
        `(std::error_code const&, typename Socket::endpoint_type const&)`
        returning a value contextually convertible to `bool`.

    @return Same as the non-condition overload. If every candidate is
        rejected, completes with `std::errc::no_such_device_or_address`.

    @throws std::system_error if auto-opening the socket fails.
*/
template<class Socket, std::ranges::input_range Range, class ConnectCondition>
    requires std::convertible_to<
                 std::ranges::range_reference_t<Range>,
                 typename Socket::endpoint_type> &&
    std::predicate<
                 ConnectCondition&,
                 std::error_code const&,
                 typename Socket::endpoint_type const&>
capy::task<capy::io_result<typename Socket::endpoint_type>>
connect(Socket& s, Range endpoints, ConnectCondition cond)
{
    using endpoint_type = typename Socket::endpoint_type;

    std::error_code last_ec;

    for (auto&& e : endpoints)
    {
        endpoint_type ep = e;

        if (!cond(static_cast<std::error_code const&>(last_ec),
                  static_cast<endpoint_type const&>(ep)))
            continue;

        if (s.is_open())
            s.close();

        auto [ec] = co_await s.connect(ep);

        if (!ec)
            co_return {std::error_code{}, std::move(ep)};

        if (ec == capy::cond::canceled)
            co_return {ec, endpoint_type{}};

        last_ec = ec;
    }

    if (!last_ec)
        last_ec = std::make_error_code(std::errc::no_such_device_or_address);

    co_return {last_ec, endpoint_type{}};
}

/** Asynchronously connect a socket by trying each endpoint in an
    iterator range.

    Behaves like the range overload, except the return value carries
    the iterator to the successfully connected endpoint on success, or
    `end` on failure. This mirrors Boost.Asio's iterator-based
    `async_connect`.

    @param s The socket to connect.
    @param begin The first candidate.
    @param end One past the last candidate.

    @return An awaitable completing with `capy::io_result<Iter>`:
        - on success: default error_code and the iterator of the
          successful endpoint;
        - on failure of all attempts: the error from the last attempt
          and `end`;
        - on empty range: `std::errc::no_such_device_or_address` and
          `end`.

    @throws std::system_error if auto-opening the socket fails.
*/
template<class Socket, std::input_iterator Iter>
    requires std::convertible_to<
        std::iter_reference_t<Iter>,
        typename Socket::endpoint_type>
capy::task<capy::io_result<Iter>>
connect(Socket& s, Iter begin, Iter end)
{
    return corosio::connect(
        s,
        std::move(begin),
        std::move(end),
        detail::default_connect_condition{});
}

/** Asynchronously connect a socket by trying each endpoint in an
    iterator range, filtered by a user-supplied condition.

    @param s The socket to connect.
    @param begin The first candidate.
    @param end One past the last candidate.
    @param cond A predicate invocable with
        `(std::error_code const&, typename Socket::endpoint_type const&)`.

    @return Same as the plain iterator overload. If every candidate is
        rejected, completes with `std::errc::no_such_device_or_address`.

    @throws std::system_error if auto-opening the socket fails.
*/
template<class Socket, std::input_iterator Iter, class ConnectCondition>
    requires std::convertible_to<
                 std::iter_reference_t<Iter>,
                 typename Socket::endpoint_type> &&
    std::predicate<
                 ConnectCondition&,
                 std::error_code const&,
                 typename Socket::endpoint_type const&>
capy::task<capy::io_result<Iter>>
connect(Socket& s, Iter begin, Iter end, ConnectCondition cond)
{
    using endpoint_type = typename Socket::endpoint_type;

    std::error_code last_ec;

    for (Iter it = begin; it != end; ++it)
    {
        endpoint_type ep = *it;

        if (!cond(static_cast<std::error_code const&>(last_ec),
                  static_cast<endpoint_type const&>(ep)))
            continue;

        if (s.is_open())
            s.close();

        auto [ec] = co_await s.connect(ep);

        if (!ec)
            co_return {std::error_code{}, std::move(it)};

        if (ec == capy::cond::canceled)
            co_return {ec, std::move(end)};

        last_ec = ec;
    }

    if (!last_ec)
        last_ec = std::make_error_code(std::errc::no_such_device_or_address);

    co_return {last_ec, std::move(end)};
}

} // namespace boost::corosio

#endif
