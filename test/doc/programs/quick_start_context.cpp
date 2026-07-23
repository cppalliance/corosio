//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// The quick-start page's first step: an io_context with no work runs
// to completion immediately.

#include <boost/corosio/io_context.hpp>

namespace corosio = boost::corosio;

// tag::full[]
int main()
{
    corosio::io_context ioc;

    // ... create and start server ...

    ioc.run();  // Process events until all work completes
}
// end::full[]
