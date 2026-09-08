//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/wait_traits.hpp's
// documentation for wait_traits, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.
//
// wait_traits is a class template (`template<class Clock>`); the reference
// slug drops the template parameter. The default instantiation needs no
// example -- it just returns its argument -- so the example shows the
// documented reason to replace it: capping how much of the remaining time a
// single steady-clock wait may cover, for a Clock (system_clock) whose
// stepped adjustments the default would otherwise observe only at the next
// natural wakeup.

#include "../doc_warnings.hpp"

#include <boost/corosio/delay.hpp>
#include <boost/corosio/wait_traits.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/task.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

// Waits a real wall-clock hour if ever launched; compiled, never run.
// tag::capped_traits[]
// Re-read the wall clock at least once per second, so a step of
// the clock is observed within that bound.
struct capped_traits
{
    static std::chrono::system_clock::duration
    to_wait_duration(std::chrono::system_clock::duration d)
    {
        return (std::min)(d,
                          std::chrono::system_clock::duration(
                              std::chrono::seconds(1)));
    }
};

// capped_traits satisfies WaitTraits<capped_traits, system_clock>, the same
// concept the default corosio::wait_traits<system_clock> satisfies -- it is
// a drop-in replacement for the default, not a different kind of thing.
static_assert(corosio::WaitTraits<capped_traits, std::chrono::system_clock>);

capy::task<>
wait_one_hour_capped()
{
    auto [ec] = co_await corosio::delay<capped_traits>(
        std::chrono::system_clock::now() + std::chrono::hours(1));
    if (ec == capy::cond::canceled)
        co_return;
    if (!ec)
        std::cout << "one wall-clock hour elapsed\n";
}
// end::capped_traits[]

} // namespace
