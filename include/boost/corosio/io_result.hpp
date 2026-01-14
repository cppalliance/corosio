//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_RESULT_HPP
#define BOOST_COROSIO_IO_RESULT_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <tuple>
#include <type_traits>

namespace boost {
namespace corosio {

/** Result type for asynchronous I/O operations.

    This template provides a unified result type for async operations,
    always containing a `system::error_code` plus optional additional
    values. It supports structured bindings and provides a `.value()`
    method that throws on error for concise exception-based code.

    @tparam Args Additional value types beyond the error code.

    @par Usage
    @code
    // Error code path (default)
    auto [ec, n] = co_await s.read_some(buf);
    if (ec) { ... }

    // Exception path (opt-in)
    auto n = (co_await s.read_some(buf)).value();
    @endcode
*/
template<class... Args>
struct io_result;

/** Result type for void operations.

    Used by operations like `connect()` that don't return a value
    beyond success/failure.

    @par Example
    @code
    auto [ec] = co_await s.connect(ep);
    if (ec) { ... }

    // Or with exceptions:
    (co_await s.connect(ep)).value();
    @endcode
*/
template<>
struct io_result<>
{
    /** The error code from the operation. */
    system::error_code ec;

    /** Check if the operation succeeded.

        @return `true` if no error occurred.
    */
    explicit operator bool() const noexcept
    {
        return !ec;
    }

    /** Throw if an error occurred.

        @throws system::system_error if `ec` is set.
    */
    void value() const
    {
        if (ec)
            detail::throw_system_error(ec);
    }

    template<std::size_t I>
    auto get() const noexcept
    {
        static_assert(I == 0, "io_result<> only has one element");
        return ec;
    }
};

/** Result type for byte transfer operations.

    Used by operations like `read_some()` and `write_some()` that
    return the number of bytes transferred.

    @par Example
    @code
    auto [ec, n] = co_await s.read_some(buf);
    if (ec) { ... }

    // Or with exceptions:
    auto n = (co_await s.read_some(buf)).value();
    @endcode
*/
template<>
struct io_result<std::size_t>
{
    /** The error code from the operation. */
    system::error_code ec;

    /** The number of bytes transferred. */
    std::size_t n = 0;

    /** Check if the operation succeeded.

        @return `true` if no error occurred.
    */
    explicit operator bool() const noexcept
    {
        return !ec;
    }

    /** Get bytes transferred, throwing on error.

        @return The number of bytes transferred.

        @throws system::system_error if `ec` is set.
    */
    std::size_t value() const
    {
        if (ec)
            detail::throw_system_error(ec);
        return n;
    }

    template<std::size_t I>
    auto get() const noexcept
    {
        if constexpr (I == 0)
            return ec;
        else
            return n;
    }
};

/** Result type for operations returning a single value.

    @tparam T The value type.
*/
template<class T>
struct io_result<T>
{
    /** The error code from the operation. */
    system::error_code ec;

    /** The result value. */
    T value_ = {};

    /** Check if the operation succeeded.

        @return `true` if no error occurred.
    */
    explicit operator bool() const noexcept
    {
        return !ec;
    }

    /** Get the value, throwing on error.

        @return The result value.

        @throws system::system_error if `ec` is set.
    */
    T value() const
    {
        if (ec)
            detail::throw_system_error(ec);
        return value_;
    }

    template<std::size_t I>
    auto get() const noexcept
    {
        if constexpr (I == 0)
            return ec;
        else
            return value_;
    }
};

/** Result type for operations returning multiple values.

    @tparam T First value type.
    @tparam U Second value type.
    @tparam Args Additional value types.
*/
template<class T, class U, class... Args>
struct io_result<T, U, Args...>
{
    /** The error code from the operation. */
    system::error_code ec;

    /** The result values. */
    std::tuple<T, U, Args...> values;

    /** Check if the operation succeeded.

        @return `true` if no error occurred.
    */
    explicit operator bool() const noexcept
    {
        return !ec;
    }

    /** Get the values, throwing on error.

        @return The result values as a tuple.

        @throws system::system_error if `ec` is set.
    */
    auto value() const ->
        std::tuple<T, U, Args...>
    {
        if (ec)
            detail::throw_system_error(ec);
        return values;
    }

    template<std::size_t I>
    auto get() const noexcept
    {
        if constexpr (I == 0)
            return ec;
        else
            return std::get<I - 1>(values);
    }
};

} // namespace corosio
} // namespace boost

template<class... Args>
struct std::tuple_size<boost::corosio::io_result<Args...>>
    : std::integral_constant<std::size_t, 1 + sizeof...(Args)> {};

template<class... Args>
struct std::tuple_element<0, boost::corosio::io_result<Args...>>
{
    using type = boost::system::error_code;
};

template<std::size_t I, class... Args>
struct std::tuple_element<I, boost::corosio::io_result<Args...>>
{
    using type = std::tuple_element_t<I - 1, std::tuple<Args...>>;
};

#endif
