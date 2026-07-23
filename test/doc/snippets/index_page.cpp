//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/index.adoc.

// tag::convention[]
#include <boost/corosio.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/ex/run_async.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::convention[]

#include "test_suite.hpp"

namespace {

struct index_page_test
{
    void
    run()
    {
        // The convention block is includes and aliases; compiling this
        // TU is the test.
        BOOST_TEST(true);
    }
};

} // namespace

TEST_SUITE(index_page_test, "boost.corosio.doc.index_page");
