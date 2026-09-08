//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/delay.hpp's
// documentation for delay, by doc/addons/extensions/reference-snippets.lua.
// The tagged region is what the reference renders; scaffolding stays outside
// the tags.
//
// Two overloads, two named regions in one file: the duration overload waits
// a fixed span; the Clock overload (shown here with system_clock, since that
// is the case the default wait_traits does not track exactly) waits for an
// arbitrary clock to reach a deadline. Both regions show the same
// cancellation contract: `error::canceled` when the environment's stop
// token wins the race, an empty error_code when the deadline is reached.

#include "../doc_warnings.hpp"

#include <boost/corosio/delay.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <iostream>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

// tag::duration[]
capy::task<>
wait_briefly()
{
    auto [ec] = co_await corosio::delay(std::chrono::milliseconds(100));
    if (ec == capy::cond::canceled)
        co_return;
    if (!ec)
        std::cout << "100ms elapsed\n";
}
// end::duration[]

// Waits a real wall-clock hour if ever launched; compiled, never run.
// tag::system_clock_deadline[]
capy::task<>
wait_one_hour_wall_clock()
{
    auto [ec] = co_await corosio::delay(
        std::chrono::system_clock::now() + std::chrono::hours(1));
    if (ec == capy::cond::canceled)
        co_return;
    if (!ec)
        std::cout << "one wall-clock hour elapsed\n";
}
// end::system_clock_deadline[]

} // namespace
