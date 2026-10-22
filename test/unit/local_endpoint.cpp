//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/local_endpoint.hpp>

#include <compare>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <system_error>

#include "test_suite.hpp"
#include "must.hpp"

namespace boost::corosio {

struct local_endpoint_test
{
    // Build an abstract-socket endpoint (leading null byte). A view
    // over a string literal would stop at the embedded null, so an
    // explicit length is required.
    static local_endpoint abstract(std::string_view name)
    {
        char buf[64] = {};
        std::memcpy(buf + 1, name.data(), name.size());
        return test::must(
            make_local_endpoint(std::string_view(buf, name.size() + 1)));
    }

    void testDefault()
    {
        local_endpoint ep;
        BOOST_TEST(ep.empty());
        BOOST_TEST(ep.path().empty());
        BOOST_TEST(!ep.is_abstract());
    }

    void testEquality()
    {
        auto a = test::must(make_local_endpoint("/tmp/sock"));
        auto b = test::must(make_local_endpoint("/tmp/sock"));
        auto c = test::must(make_local_endpoint("/tmp/other"));
        auto d = test::must(
            make_local_endpoint("/tmp/sock2")); // longer, shares prefix

        BOOST_TEST(a == b);
        BOOST_TEST(!(a != b));
        BOOST_TEST(a != c);
        BOOST_TEST(a != d);
    }

    void testOrdering()
    {
        // Empty sorts before any non-empty path.
        local_endpoint empty;
        auto a = test::must(make_local_endpoint("a"));
        BOOST_TEST(empty < a);
        BOOST_TEST(a > empty);

        // A prefix sorts before the longer path that extends it.
        auto ab = test::must(make_local_endpoint("ab"));
        BOOST_TEST(a < ab);
        BOOST_TEST(ab > a);

        // Differing bytes order lexicographically.
        auto b = test::must(make_local_endpoint("b"));
        BOOST_TEST(a < b);
        BOOST_TEST(b > a);

        // Equal paths compare equivalent.
        auto a2 = test::must(make_local_endpoint("a"));
        BOOST_TEST((a <=> a2) == std::strong_ordering::equal);
        BOOST_TEST(!(a < a2));
        BOOST_TEST(!(a > a2));
        BOOST_TEST(a <= a2);
        BOOST_TEST(a >= a2);
    }

    void testAbstract()
    {
        local_endpoint abs = abstract("foo");
        BOOST_TEST(abs.is_abstract());
        BOOST_TEST_EQ(abs.path().size(), 4u); // leading '\0' + "foo"

        // The leading null byte sorts abstract paths before any
        // ordinary path (whose first byte is non-null).
        auto ordinary = test::must(make_local_endpoint("a"));
        BOOST_TEST(abs < ordinary);
        BOOST_TEST(ordinary > abs);

        // Two abstract endpoints order by their name bytes.
        local_endpoint abs_a = abstract("a");
        local_endpoint abs_b = abstract("b");
        BOOST_TEST(abs_a < abs_b);
        BOOST_TEST(abs_a != abs_b);
    }

    void testMakeLocalEndpoint()
    {
        std::string at_limit(local_endpoint::max_path_length, 'a');
        auto [ec, ep] = make_local_endpoint(at_limit);
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(ep.path(), at_limit);

        std::string over(local_endpoint::max_path_length + 1, 'a');
        auto [oec, oep] = make_local_endpoint(over);
        BOOST_TEST(oec == std::errc::filename_too_long);
        BOOST_TEST(oep.empty());

        auto [eec, eep] = make_local_endpoint("");
        BOOST_TEST(!eec);
        BOOST_TEST(eep.empty());

        auto [nec, nep] = make_local_endpoint(std::string_view{});
        BOOST_TEST(!nec);
        BOOST_TEST(nep.empty());
    }

    void testOrderedMapKey()
    {
        std::map<local_endpoint, int> sessions;
        sessions[test::must(make_local_endpoint("/tmp/a"))] = 1;
        sessions[test::must(make_local_endpoint("/tmp/b"))] = 2;
        sessions[abstract("c")]                             = 3;

        BOOST_TEST_EQ(sessions.size(), 3u);
        BOOST_TEST_EQ(sessions[test::must(make_local_endpoint("/tmp/a"))], 1);

        auto it = sessions.find(test::must(make_local_endpoint("/tmp/b")));
        BOOST_TEST(it != sessions.end());
        BOOST_TEST_EQ(it->second, 2);

        // The abstract key (leading '\0') sorts first.
        BOOST_TEST(sessions.begin()->first.is_abstract());
    }

    void run()
    {
        testDefault();
        testEquality();
        testOrdering();
        testAbstract();
        testMakeLocalEndpoint();
        testOrderedMapKey();
    }
};

TEST_SUITE(local_endpoint_test, "boost.corosio.local_endpoint");

} // namespace boost::corosio
