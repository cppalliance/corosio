//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/native/detail/make_err.hpp>

#include <system_error>

#include "test_suite.hpp"

namespace boost::corosio {

struct make_err_test
{
    // The zero-value success arm is the same on every platform: a zero
    // errno (POSIX) or Windows error code maps to an empty error_code.
    // The non-zero arms are driven elsewhere (fault injection, the IOCP
    // error map); this pins the success path directly.
    void testSuccess()
    {
        auto ec = detail::make_err(0);
        BOOST_TEST(!ec);
        BOOST_TEST(ec == std::error_code{});
    }

    void run()
    {
        testSuccess();
    }
};

TEST_SUITE(make_err_test, "boost.corosio.native.make_err");

} // namespace boost::corosio
