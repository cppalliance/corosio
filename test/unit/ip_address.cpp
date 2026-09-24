//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/ip_address.hpp>

#include <sstream>
#include <unordered_set>
#include <system_error>

#include "test_suite.hpp"

namespace boost::corosio {

struct ip_address_test
{
    void testConstruction()
    {
        // Default construction is the IPv4 any address
        {
            ip_address a;
            BOOST_TEST(a.is_v4());
            BOOST_TEST(!a.is_v6());
            BOOST_TEST(a.is_unspecified());
        }

        // Implicit conversion from ipv4_address
        {
            ip_address a = ipv4_address::loopback();
            BOOST_TEST(a.is_v4());
            BOOST_TEST_EQ(a.to_v4(), ipv4_address::loopback());
        }

        // Implicit conversion from ipv6_address
        {
            ip_address a = ipv6_address::loopback();
            BOOST_TEST(a.is_v6());
            BOOST_TEST(!a.is_v4());
            BOOST_TEST_EQ(a.to_v6(), ipv6_address::loopback());
        }

        // Construct from string, both families
        {
            ip_address a("192.168.1.1");
            BOOST_TEST(a.is_v4());
            BOOST_TEST_EQ(a.to_string(), "192.168.1.1");

            ip_address b("2001:db8::1");
            BOOST_TEST(b.is_v6());
            BOOST_TEST_EQ(b.to_string(), "2001:db8::1");
        }

        // Invalid string throws system_error carrying the parse code
        {
            BOOST_TEST_THROWS(ip_address("invalid"), std::system_error);
            try
            {
                ip_address("invalid");
                BOOST_TEST_FAIL();
            }
            catch (std::system_error const& e)
            {
                BOOST_TEST(e.code() == std::errc::invalid_argument);
            }
        }
    }

    void testFamily()
    {
        // The enum is the portable spelling; the predicates are
        // sugar over it
        BOOST_TEST(ip_address().family() == family::v4);
        BOOST_TEST(ip_address(ipv4_address::loopback()).family() == family::v4);
        BOOST_TEST(ip_address(ipv6_address::loopback()).family() == family::v6);
        BOOST_TEST(ip_address("fe80::1%2").family() == family::v6);
    }

    void testPredicates()
    {
        // Loopback dispatches per family
        BOOST_TEST(ip_address(ipv4_address::loopback()).is_loopback());
        BOOST_TEST(ip_address(ipv6_address::loopback()).is_loopback());
        BOOST_TEST(!ip_address(ipv4_address::any()).is_loopback());
        BOOST_TEST(!ip_address(ipv6_address::any()).is_loopback());

        // Unspecified dispatches per family
        BOOST_TEST(ip_address(ipv4_address::any()).is_unspecified());
        BOOST_TEST(ip_address(ipv6_address::any()).is_unspecified());
        BOOST_TEST(!ip_address(ipv4_address::loopback()).is_unspecified());
        BOOST_TEST(!ip_address(ipv6_address::loopback()).is_unspecified());

        // Multicast dispatches per family
        BOOST_TEST(ip_address(ipv4_address(0xE0000001)).is_multicast());
        BOOST_TEST(ip_address(ipv6_address("ff02::1")).is_multicast());
        BOOST_TEST(!ip_address(ipv4_address::loopback()).is_multicast());
        BOOST_TEST(!ip_address(ipv6_address::loopback()).is_multicast());

        // v4-mapped is a v6-family property
        ipv4_address v4(0xC0A80101);
        BOOST_TEST(ip_address(ipv6_address(v4)).is_v4_mapped());
        BOOST_TEST(!ip_address(v4).is_v4_mapped());
        BOOST_TEST(!ip_address(ipv6_address::loopback()).is_v4_mapped());
    }

    void testConversion()
    {
        ipv4_address v4(0xC0A80101); // 192.168.1.1

        // to_v4 on a v4-family address
        BOOST_TEST_EQ(ip_address(v4).to_v4(), v4);

        // to_v4 unmaps a v4-mapped v6 address
        BOOST_TEST_EQ(ip_address(ipv6_address(v4)).to_v4(), v4);

        // to_v4 on a plain v6 address refuses the conversion
        BOOST_TEST_THROWS(
            ip_address(ipv6_address::loopback()).to_v4(), std::system_error);
        try
        {
            ip_address(ipv6_address::loopback()).to_v4();
            BOOST_TEST_FAIL();
        }
        catch (std::system_error const& e)
        {
            BOOST_TEST(e.code() == std::errc::address_family_not_supported);
        }

        // to_v6 on a v6-family address
        BOOST_TEST_EQ(
            ip_address(ipv6_address::loopback()).to_v6(),
            ipv6_address::loopback());

        // to_v6 on a v4-family address refuses the conversion
        BOOST_TEST_THROWS(ip_address(v4).to_v6(), std::system_error);
        try
        {
            ip_address(v4).to_v6();
            BOOST_TEST_FAIL();
        }
        catch (std::system_error const& e)
        {
            BOOST_TEST(e.code() == std::errc::address_family_not_supported);
        }
    }

    void testV4MappedSemantics()
    {
        ipv4_address v4(0xC0A80101);
        ip_address plain(v4);
        ip_address mapped((ipv6_address(v4)));

        // A v4-mapped address is not equal to its plain v4 form
        BOOST_TEST(plain != mapped);
        BOOST_TEST(plain.is_v4());
        BOOST_TEST(mapped.is_v6());

        // Normalizing through to_v4 recovers equality
        BOOST_TEST_EQ(plain.to_v4(), mapped.to_v4());
    }

    void testScopeIdTransitive()
    {
        // The zone rides along through the family-generic type
        auto [ec, a] = make_ip_address("fe80::1%2");
        BOOST_TEST(!ec);
        BOOST_TEST(a.is_v6());
        BOOST_TEST_EQ(a.to_v6().scope_id(), 2u);
        BOOST_TEST_EQ(a.to_string(), "fe80::1%2");

        // Same bits on different links are different values
        auto [ec2, b] = make_ip_address("fe80::1%3");
        auto [ec3, c] = make_ip_address("fe80::1");
        BOOST_TEST(!ec2);
        BOOST_TEST(!ec3);
        BOOST_TEST(a != b);
        BOOST_TEST(a != c);
        BOOST_TEST(c < a);
        BOOST_TEST(a < b);
    }

    void testParse()
    {
        // Valid addresses, both families
        auto check_valid = [](std::string_view s, bool v4) {
            auto [ec, addr] = make_ip_address(s);
            if (!BOOST_TEST(!ec))
                return;
            BOOST_TEST_EQ(addr.is_v4(), v4);
        };

        check_valid("0.0.0.0", true);
        check_valid("127.0.0.1", true);
        check_valid("255.255.255.255", true);
        check_valid("::", false);
        check_valid("::1", false);
        check_valid("2001:db8::1", false);
        check_valid("1:2:3:4:5:6:7:8", false);
        check_valid("::ffff:192.168.1.1", false); // mapped parses as v6

        // Invalid addresses: no ports, no brackets, no hostnames
        auto check_invalid = [](std::string_view s) {
            auto [ec, addr] = make_ip_address(s);
            BOOST_TEST(ec == std::errc::invalid_argument);
            // The failure payload is the documented default value.
            BOOST_TEST(addr == ip_address());
        };

        check_invalid("");
        check_invalid("192.168.1.1:8080");
        check_invalid("[::1]");
        check_invalid("[::1]:8080");
        check_invalid("example.com");
        check_invalid("192.168.1.1x");
        check_invalid("::1x");
        check_invalid(" ::1");
        check_invalid("1.2.3");
    }

    void testComparison()
    {
        ip_address v4_lo(ipv4_address::loopback());
        ip_address v6_lo(ipv6_address::loopback());

        BOOST_TEST(v4_lo == ip_address(ipv4_address::loopback()));
        BOOST_TEST(v6_lo == ip_address(ipv6_address::loopback()));
        BOOST_TEST(v4_lo != v6_lo);
        BOOST_TEST(v4_lo != ip_address(ipv4_address::any()));

        // Family alone must separate values whose inactive members
        // coincide: both wildcards, and v4-any against its mapped form
        BOOST_TEST(
            ip_address(ipv4_address::any()) != ip_address(ipv6_address::any()));
        BOOST_TEST(
            ip_address(ipv4_address::any()) !=
            ip_address(ipv6_address(ipv4_address::any())));

        // Distinct v6 values must compare unequal through operator==
        BOOST_TEST(v6_lo != ip_address(ipv6_address::any()));
    }

    void testOrdering()
    {
        ip_address v4_any(ipv4_address::any());
        ip_address v4_hi(ipv4_address::broadcast());
        ip_address v6_any(ipv6_address::any());
        ip_address v6_lo(ipv6_address::loopback());

        // v4 orders before v6, regardless of value
        BOOST_TEST(v4_hi < v6_any);
        BOOST_TEST(v6_any > v4_hi);

        // Within a family, order follows the address value
        BOOST_TEST(v4_any < v4_hi);
        BOOST_TEST(v6_any < v6_lo);

        // Consistency with equality
        BOOST_TEST(
            (v4_any <=> ip_address(ipv4_address::any())) ==
            std::strong_ordering::equal);
        BOOST_TEST(
            (v6_lo <=> ip_address(ipv6_address::loopback())) ==
            std::strong_ordering::equal);
    }

    void testToString()
    {
        BOOST_TEST_EQ(
            ip_address(ipv4_address(0xC0A80101)).to_string(), "192.168.1.1");
        BOOST_TEST_EQ(ip_address(ipv6_address::loopback()).to_string(), "::1");
    }

    void testToBuffer()
    {
        char buf[ip_address::max_str_len];
        auto sv =
            ip_address(ipv4_address(0x01020304)).to_buffer(buf, sizeof(buf));
        BOOST_TEST_EQ(sv, "1.2.3.4");

        auto sv6 =
            ip_address(ipv6_address::loopback()).to_buffer(buf, sizeof(buf));
        BOOST_TEST_EQ(sv6, "::1");

        // The capacity floor is family-independent: a held v4 value
        // still requires max_str_len, so one byte short must throw
        // even though the v4 formatter alone would accept it
        char shy[ip_address::max_str_len - 1];
        BOOST_TEST_THROWS(
            ip_address(ipv4_address(0x01020304)).to_buffer(shy, sizeof(shy)),
            std::length_error);
    }

    void testHash()
    {
        // Equal values hash equal across construction paths
        BOOST_TEST_EQ(
            std::hash<ip_address>()(ip_address(ipv4_address::loopback())),
            std::hash<ip_address>()(ip_address("127.0.0.1")));

        // Family, mapping, and zone all separate keys
        std::unordered_set<ip_address> set;
        set.insert(ip_address(ipv4_address::any()));
        set.insert(ip_address(ipv6_address::any()));
        set.insert(ip_address(ipv6_address(ipv4_address::any())));
        set.insert(ip_address("fe80::1%2"));
        set.insert(ip_address("fe80::1"));
        set.insert(ip_address(ipv4_address::any()));
        BOOST_TEST_EQ(set.size(), 5u);
        BOOST_TEST(set.contains(ip_address("fe80::1%2")));
    }

    void testOstream()
    {
        std::ostringstream oss;
        oss << ip_address(ipv4_address(0xC0A80101)) << " "
            << ip_address(ipv6_address::loopback());
        BOOST_TEST_EQ(oss.str(), "192.168.1.1 ::1");
    }

    void run()
    {
        testConstruction();
        testFamily();
        testPredicates();
        testConversion();
        testV4MappedSemantics();
        testScopeIdTransitive();
        testParse();
        testComparison();
        testOrdering();
        testToString();
        testToBuffer();
        testHash();
        testOstream();
    }
};

TEST_SUITE(ip_address_test, "boost.corosio.ip_address");

} // namespace boost::corosio
