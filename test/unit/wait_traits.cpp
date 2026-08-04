//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/wait_traits.hpp>

#include <chrono>
#include <ratio>

#include "test_suite.hpp"

namespace boost::corosio {

struct wait_traits_test
{
    struct minute_clock
    {
        using rep        = int;
        using period     = std::ratio<60>;
        using duration   = std::chrono::duration<rep, period>;
        using time_point = std::chrono::time_point<minute_clock>;
        static constexpr bool is_steady = false;
        static time_point now() noexcept { return {}; }
    };

    struct capped
    {
        static std::chrono::system_clock::duration
        to_wait_duration(std::chrono::system_clock::duration d)
        {
            return (std::min)(d,
                std::chrono::system_clock::duration(
                    std::chrono::seconds(1)));
        }
    };

    struct not_traits
    {
    };

    void testDefaultIsIdentity()
    {
        auto d = std::chrono::system_clock::duration(
            std::chrono::seconds(5));
        BOOST_TEST(wait_traits<
            std::chrono::system_clock>::to_wait_duration(d) == d);
        BOOST_TEST(wait_traits<minute_clock>::to_wait_duration(
            minute_clock::duration(3)) == minute_clock::duration(3));
        BOOST_TEST(wait_traits<minute_clock>::to_wait_duration(
            minute_clock::duration(-3)) == minute_clock::duration(-3));
    }

    void testConcept()
    {
        static_assert(WaitTraits<
            wait_traits<std::chrono::system_clock>,
            std::chrono::system_clock>);
        static_assert(WaitTraits<
            wait_traits<minute_clock>, minute_clock>);
        static_assert(WaitTraits<capped, std::chrono::system_clock>);
        static_assert(!WaitTraits<not_traits, std::chrono::system_clock>);
        static_assert(!WaitTraits<capped, minute_clock>);
    }

    void run()
    {
        testDefaultIsIdentity();
        testConcept();
    }
};

TEST_SUITE(wait_traits_test, "boost.corosio.wait_traits");

} // namespace boost::corosio
