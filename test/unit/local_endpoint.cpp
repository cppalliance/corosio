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
        return local_endpoint(std::string_view(buf, name.size() + 1));
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
        local_endpoint a("/tmp/sock");
        local_endpoint b("/tmp/sock");
        local_endpoint c("/tmp/other");
        local_endpoint d("/tmp/sock2"); // longer, shares prefix

        BOOST_TEST(a == b);
        BOOST_TEST(!(a != b));
        BOOST_TEST(a != c);
        BOOST_TEST(a != d);
    }

    void testOrdering()
    {
        // Empty sorts before any non-empty path.
        local_endpoint empty;
        local_endpoint a("a");
        BOOST_TEST(empty < a);
        BOOST_TEST(a > empty);

        // A prefix sorts before the longer path that extends it.
        local_endpoint ab("ab");
        BOOST_TEST(a < ab);
        BOOST_TEST(ab > a);

        // Differing bytes order lexicographically.
        local_endpoint b("b");
        BOOST_TEST(a < b);
        BOOST_TEST(b > a);

        // Equal paths compare equivalent.
        local_endpoint a2("a");
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
        local_endpoint ordinary("a");
        BOOST_TEST(abs < ordinary);
        BOOST_TEST(ordinary > abs);

        // Two abstract endpoints order by their name bytes.
        local_endpoint abs_a = abstract("a");
        local_endpoint abs_b = abstract("b");
        BOOST_TEST(abs_a < abs_b);
        BOOST_TEST(abs_a != abs_b);
    }

    void testTooLongThrows()
    {
        std::string too_long(local_endpoint::max_path_length + 1, 'x');
        BOOST_TEST_THROWS(local_endpoint(too_long), std::system_error);

        std::error_code ec;
        local_endpoint ep(too_long, ec);
        BOOST_TEST(bool(ec));
        BOOST_TEST(ep.empty());

        // The maximum-length path is accepted.
        std::string at_max(local_endpoint::max_path_length, 'x');
        std::error_code ec2;
        local_endpoint ok(at_max, ec2);
        BOOST_TEST(!ec2);
        BOOST_TEST_EQ(ok.path().size(), local_endpoint::max_path_length);
    }

    void testOrderedMapKey()
    {
        std::map<local_endpoint, int> sessions;
        sessions[local_endpoint("/tmp/a")] = 1;
        sessions[local_endpoint("/tmp/b")] = 2;
        sessions[abstract("c")] = 3;

        BOOST_TEST_EQ(sessions.size(), 3u);
        BOOST_TEST_EQ(sessions[local_endpoint("/tmp/a")], 1);

        auto it = sessions.find(local_endpoint("/tmp/b"));
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
        testTooLongThrows();
        testOrderedMapKey();
    }
};

TEST_SUITE(local_endpoint_test, "boost.corosio.local_endpoint");

} // namespace boost::corosio
