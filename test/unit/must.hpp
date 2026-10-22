//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_UNIT_MUST_HPP
#define BOOST_COROSIO_TEST_UNIT_MUST_HPP

#include <boost/capy/results.hpp>

#include "test_suite.hpp"

#include <system_error>
#include <tuple>
#include <utility>

namespace boost::corosio::test {

/** Return the payload of a setup step that must succeed.

    Records a test failure and throws `std::system_error` on error:
    a fixture that cannot be built leaves nothing meaningful to test.
*/
template<class T>
T
must(capy::results<T>&& r)
{
    auto& [ec, v] = r;
    BOOST_TEST(!ec);
    if (ec)
        throw std::system_error(ec);
    return std::move(v);
}

/// Return the payloads of a multi-value setup step that must succeed.
template<class T1, class T2, class... Ts>
std::tuple<T1, T2, Ts...>
must(capy::results<T1, T2, Ts...>&& r)
{
    if (auto ec = std::get<0>(r))
    {
        BOOST_TEST(!ec);
        throw std::system_error(ec);
    }
    return std::apply(
        [](std::error_code, auto&&... vs) {
            return std::tuple<T1, T2, Ts...>(std::move(vs)...);
        },
        std::move(r));
}

/// Check a setup step that must succeed.
inline void
must_ok(std::error_code ec)
{
    BOOST_TEST(!ec);
    if (ec)
        throw std::system_error(ec);
}

} // namespace boost::corosio::test

#endif
