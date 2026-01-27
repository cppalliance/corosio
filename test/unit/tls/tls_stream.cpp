//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/tls/tls_stream.hpp>

#include "test_suite.hpp"

namespace boost::corosio {

struct tls_stream_test
{
    void
    run()
    {
    }
};

TEST_SUITE(tls_stream_test, "boost.corosio.tls_stream");

} // namespace boost::corosio
