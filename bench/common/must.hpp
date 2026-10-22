//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_BENCH_COMMON_MUST_HPP
#define BOOST_COROSIO_BENCH_COMMON_MUST_HPP

#include <boost/capy/results.hpp>

#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <tuple>
#include <utility>

namespace bench {

/// Check a setup step that must succeed, aborting otherwise.
inline void
must_ok(std::error_code ec)
{
    if (ec)
    {
        std::fprintf(stderr, "bench setup failed: %s\n", ec.message().c_str());
        std::abort();
    }
}

/// Return the payload of a setup step that must succeed.
template<class T>
T
must(boost::capy::results<T>&& r)
{
    must_ok(std::get<0>(r));
    return std::move(std::get<1>(r));
}

/// Return the payloads of a multi-value setup step that must succeed.
template<class T1, class T2, class... Ts>
std::tuple<T1, T2, Ts...>
must(boost::capy::results<T1, T2, Ts...>&& r)
{
    must_ok(std::get<0>(r));
    return std::apply(
        [](std::error_code, auto&&... vs) {
            return std::tuple<T1, T2, Ts...>(std::move(vs)...);
        },
        std::move(r));
}

} // namespace bench

#endif
