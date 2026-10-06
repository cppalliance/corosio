//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_ERROR_HPP
#define BOOST_COROSIO_ERROR_HPP

#include <boost/corosio/detail/config.hpp>

#include <string>
#include <system_error>
#include <type_traits>

namespace boost::corosio {

/** Identifies failures corosio detects itself.

    Codes the operating system reports keep their system category.
*/
enum class error
{
    /// The target object already holds an open handle.
    already_open = 1
};

} // namespace boost::corosio

namespace std {
template<>
struct is_error_code_enum<::boost::corosio::error> : std::true_type
{};
} // namespace std

namespace boost::corosio {
namespace detail {

struct BOOST_COROSIO_SYMBOL_VISIBLE error_cat_type : std::error_category
{
    BOOST_COROSIO_DECL char const* name() const noexcept override;
    BOOST_COROSIO_DECL std::string message(int) const override;
    constexpr error_cat_type() noexcept = default;
};

BOOST_COROSIO_DECL extern error_cat_type error_cat;

} // namespace detail

/** Create an `std::error_code` from a corosio error value.

    @param ev The error value.
    @return An `std::error_code` in corosio's category.
*/
inline std::error_code
make_error_code(error ev) noexcept
{
    return std::error_code{static_cast<int>(ev), detail::error_cat};
}

} // namespace boost::corosio

#endif
