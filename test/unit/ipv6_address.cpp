//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/ipv6_address.hpp>
#include <boost/corosio/ipv4_address.hpp>

#include <sstream>

#include "test_suite.hpp"

namespace boost::corosio {

struct ipv6_address_test
{
    void testConstruction()
    {
        // Default construction (unspecified)
        {
            ipv6_address a;
            BOOST_TEST(a.is_unspecified());
        }

        // Construct from bytes
        {
            ipv6_address::bytes_type bytes{
                {0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7, 0, 8}};
            ipv6_address a(bytes);
            BOOST_TEST_EQ(a.to_string(), "1:2:3:4:5:6:7:8");
        }

        // Construct from IPv4 address (mapped)
        {
            ipv4_address v4(0xC0A80101); // 192.168.1.1
            ipv6_address a(v4);
            BOOST_TEST(a.is_v4_mapped());
            BOOST_TEST_EQ(a.to_string(), "::ffff:192.168.1.1");
        }

        // Construct from string
        {
            ipv6_address a("::1");
            BOOST_TEST(a.is_loopback());
        }

        // Invalid string throws
        {
            BOOST_TEST_THROWS(ipv6_address("invalid"), std::invalid_argument);
            BOOST_TEST_THROWS(ipv6_address(":::1"), std::invalid_argument);
        }
    }

    void testParse()
    {
        // Valid addresses
        auto check_valid = [](std::string_view s) {
            ipv6_address addr;
            auto ec = parse_ipv6_address(s, addr);
            if (ec)
            {
                BOOST_TEST_FAIL();
                return;
            }
            BOOST_TEST_PASS();
        };

        // Basic cases
        check_valid("::");
        check_valid("::1");
        check_valid("1::");
        check_valid("1::1");
        check_valid("1:2:3:4:5:6:7:8");
        check_valid("2001:db8::1");
        check_valid("fe80::1");
        check_valid("::ffff:192.168.1.1");

        // Various :: positions
        check_valid("1::8");
        check_valid("1:2::8");
        check_valid("1:2:3::8");
        check_valid("1:2:3:4::8");
        check_valid("1:2:3:4:5::8");
        check_valid("1:2:3:4:5:6::8");
        check_valid("1::3:4:5:6:7:8");
        check_valid("::2:3:4:5:6:7:8");

        // IPv4-mapped
        check_valid("::192.168.1.1");
        check_valid("::ffff:10.0.0.1");
        check_valid("::1:192.168.1.1");

        // Invalid addresses
        auto check_invalid = [](std::string_view s) {
            ipv6_address addr;
            auto ec = parse_ipv6_address(s, addr);
            BOOST_TEST(bool(ec));
        };

        check_invalid("");
        check_invalid(":");
        check_invalid(":::");
        check_invalid(":::1");
        check_invalid("1:::");
        check_invalid("1:::1");
        check_invalid("1:2:3:4:5:6:7:8:9");       // too many groups
        check_invalid("1:2:3:4:5:6:7");           // too few groups (no ::)
        check_invalid("1::2::3");                 // multiple ::
        check_invalid("12345::");                 // segment too large
        check_invalid("g::");                     // invalid hex
        check_invalid("1:2:3:4:5:6:7:256.0.0.0"); // invalid IPv4
    }

    void testToString()
    {
        // Unspecified
        BOOST_TEST_EQ(ipv6_address().to_string(), "::");

        // Loopback
        BOOST_TEST_EQ(ipv6_address::loopback().to_string(), "::1");

        // Full address
        {
            ipv6_address::bytes_type bytes{
                {0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7, 0, 8}};
            ipv6_address a(bytes);
            BOOST_TEST_EQ(a.to_string(), "1:2:3:4:5:6:7:8");
        }

        // IPv4-mapped
        {
            ipv4_address v4(0x7F000001);
            ipv6_address a(v4);
            BOOST_TEST_EQ(a.to_string(), "::ffff:127.0.0.1");
        }
    }

    void testToBuffer()
    {
        char buf[ipv6_address::max_str_len];
        auto sv = ipv6_address::loopback().to_buffer(buf, sizeof(buf));
        BOOST_TEST_EQ(sv, "::1");
    }

    void testToBufferTooSmallThrows()
    {
        // to_buffer must throw length_error when the buffer is smaller
        // than max_str_len, even if the formatted address would fit.
        char small[4];
        BOOST_TEST_THROWS(
            ipv6_address::loopback().to_buffer(small, sizeof(small)),
            std::length_error);
    }

    void testToStringHexWidths()
    {
        // Exercise each print_hex width branch (4, 3, 2, 1 hex digit).
        // 4 digits: 0xabcd, 3 digits: 0x0bcd, 2 digits: 0x00bc, 1 digit: 0x000b.
        ipv6_address::bytes_type b{
            {0xab, 0xcd, 0x0b, 0xcd, 0x00, 0xbc, 0x00, 0x0b,
             0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0}};
        ipv6_address a(b);
        BOOST_TEST_EQ(a.to_string(), "abcd:bcd:bc:b:1234:5678:9abc:def0");
    }

    void testParseEndsWithDoubleColon()
    {
        // "1::" — '::' at the end requires the "ends in ::" hex break path.
        ipv6_address addr;
        auto ec = parse_ipv6_address("1::", addr);
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(addr.to_string(), "1::");
    }

    void testParseInvalidIPv4Suffix()
    {
        ipv6_address addr;
        // "::1.2.3" — IPv4 portion incomplete.
        BOOST_TEST(parse_ipv6_address("::1.2.3", addr));
        // "::g.0.0.0" — non-numeric hex.
        BOOST_TEST(parse_ipv6_address("::g.0.0.0", addr));
        // "1:2:3:4:5:6.7.8.9" — IPv4 with no '::' but not enough h16 groups.
        BOOST_TEST(parse_ipv6_address("1:2:3:4:5:6.7.8.9", addr));
        // The embedded-IPv4 validator parses each octet as an h16 and
        // rejects values dotted decimal can never produce.
        BOOST_TEST(parse_ipv6_address("::1.2.3.400", addr));
        BOOST_TEST(parse_ipv6_address("::1.2.3.2a", addr));
        BOOST_TEST(parse_ipv6_address("::1.2.3.a1", addr));
    }

    void testParseMoreEdges()
    {
        ipv6_address addr;

        // Uppercase hex digits.
        BOOST_TEST(!parse_ipv6_address("ABCD::EF01", addr));
        BOOST_TEST_EQ(addr.to_string(), "abcd::ef01");

        // Full-form embedded IPv4 with no '::'.
        BOOST_TEST(!parse_ipv6_address("1:2:3:4:5:6:1.2.3.4", addr));

        // Input ending right after a colon.
        BOOST_TEST(parse_ipv6_address("1:", addr));
        BOOST_TEST(parse_ipv6_address("1:2:3:4:5:6:7:", addr));

        // Non-hex garbage after '::'.
        BOOST_TEST(parse_ipv6_address("1::zz", addr));

        // '::' with all eight groups already present.
        BOOST_TEST(parse_ipv6_address("1:2:3:4:5:6:7:8::", addr));

        // '::' compressing exactly zero remaining groups at the end.
        BOOST_TEST(!parse_ipv6_address("1:2:3:4:5:6:7::", addr));
    }

    void testPredicates()
    {
        // Unspecified
        BOOST_TEST(ipv6_address().is_unspecified());
        BOOST_TEST(!ipv6_address::loopback().is_unspecified());

        // Loopback
        BOOST_TEST(ipv6_address::loopback().is_loopback());
        BOOST_TEST(!ipv6_address().is_loopback());

        // IPv4-mapped
        {
            ipv4_address v4(0xC0A80101);
            ipv6_address a(v4);
            BOOST_TEST(a.is_v4_mapped());
        }
        BOOST_TEST(!ipv6_address().is_v4_mapped());
        BOOST_TEST(!ipv6_address::loopback().is_v4_mapped());

        // Multicast
        {
            ipv6_address a("ff02::1");
            BOOST_TEST(a.is_multicast());
        }
        {
            ipv6_address a("ff05::1:3");
            BOOST_TEST(a.is_multicast());
        }
        BOOST_TEST(!ipv6_address().is_multicast());
        BOOST_TEST(!ipv6_address::loopback().is_multicast());
    }

    void testComparison()
    {
        ipv6_address a1 = ipv6_address::loopback();
        ipv6_address a2 = ipv6_address::loopback();
        ipv6_address a3;

        BOOST_TEST(a1 == a2);
        BOOST_TEST(!(a1 != a2));
        BOOST_TEST(a1 != a3);
        BOOST_TEST(!(a1 == a3));
    }

    void testOstream()
    {
        std::ostringstream oss;
        oss << ipv6_address::loopback();
        BOOST_TEST_EQ(oss.str(), "::1");
    }

    void run()
    {
        testConstruction();
        testParse();
        testParseEndsWithDoubleColon();
        testParseInvalidIPv4Suffix();
        testParseMoreEdges();
        testToString();
        testToStringHexWidths();
        testToBuffer();
        testToBufferTooSmallThrows();
        testPredicates();
        testComparison();
        testOstream();
    }
};

TEST_SUITE(ipv6_address_test, "boost.corosio.ipv6_address");

} // namespace boost::corosio
