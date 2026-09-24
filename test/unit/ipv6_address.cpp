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

#include <boost/corosio/detail/platform.hpp>

#include <sstream>
#include <unordered_set>
#include <string>
#include <tuple>
#include <system_error>

#if BOOST_COROSIO_POSIX
#include <net/if.h>
#endif

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

        // Invalid string throws system_error carrying the parse code
        {
            BOOST_TEST_THROWS(ipv6_address("invalid"), std::system_error);
            BOOST_TEST_THROWS(ipv6_address(":::1"), std::system_error);
            try
            {
                ipv6_address("invalid");
                BOOST_TEST_FAIL();
            }
            catch (std::system_error const& e)
            {
                BOOST_TEST(e.code() == std::errc::invalid_argument);
            }
        }
    }

    void testParse()
    {
        // Valid addresses
        auto check_valid = [](std::string_view s) {
            auto [ec, addr] = make_ipv6_address(s);
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
            auto [ec, addr] = make_ipv6_address(s);
            BOOST_TEST(ec == std::errc::invalid_argument);
            // The failure payload is the documented default value.
            BOOST_TEST(addr == ipv6_address());
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
            {0xab, 0xcd, 0x0b, 0xcd, 0x00, 0xbc, 0x00, 0x0b, 0x12, 0x34, 0x56,
             0x78, 0x9a, 0xbc, 0xde, 0xf0}};
        ipv6_address a(b);
        BOOST_TEST_EQ(a.to_string(), "abcd:bcd:bc:b:1234:5678:9abc:def0");
    }

    void testParseEndsWithDoubleColon()
    {
        // "1::" — '::' at the end requires the "ends in ::" hex break path.
        auto [ec, addr] = make_ipv6_address("1::");
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(addr.to_string(), "1::");
    }

    void testParseInvalidIPv4Suffix()
    {
        auto rejects = [](std::string_view s) {
            return bool(std::get<0>(make_ipv6_address(s)));
        };
        // "::1.2.3" — IPv4 portion incomplete.
        BOOST_TEST(rejects("::1.2.3"));
        // "::g.0.0.0" — non-numeric hex.
        BOOST_TEST(rejects("::g.0.0.0"));
        // "1:2:3:4:5:6.7.8.9" — IPv4 with no '::' but not enough h16 groups.
        BOOST_TEST(rejects("1:2:3:4:5:6.7.8.9"));
        // The embedded-IPv4 validator parses each octet as an h16 and
        // rejects values dotted decimal can never produce.
        BOOST_TEST(rejects("::1.2.3.400"));
        BOOST_TEST(rejects("::1.2.3.2a"));
        BOOST_TEST(rejects("::1.2.3.a1"));
        BOOST_TEST(rejects("::1.2.3.1a1"));
        BOOST_TEST(rejects("1:zz::"));
    }

    void testParseMoreEdges()
    {
        auto rejects = [](std::string_view s) {
            return bool(std::get<0>(make_ipv6_address(s)));
        };

        // Uppercase hex digits.
        auto [uec, upper] = make_ipv6_address("ABCD::EF01");
        BOOST_TEST(!uec);
        BOOST_TEST_EQ(upper.to_string(), "abcd::ef01");

        // Full-form embedded IPv4 with no '::'.
        BOOST_TEST(!rejects("1:2:3:4:5:6:1.2.3.4"));

        // Input ending right after a colon.
        BOOST_TEST(rejects("1:"));
        BOOST_TEST(rejects("1:2:3:4:5:6:7:"));

        // Non-hex garbage after '::'.
        BOOST_TEST(rejects("1::zz"));

        // '::' with all eight groups already present.
        BOOST_TEST(rejects("1:2:3:4:5:6:7:8::"));

        // '::' compressing exactly zero remaining groups at the end.
        BOOST_TEST(!rejects("1:2:3:4:5:6:7::"));
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

    void testOrdering()
    {
        auto a1 = ipv6_address::loopback();
        auto a2 = ipv6_address::loopback();

        BOOST_TEST((a1 <=> a2) == std::strong_ordering::equal);
        BOOST_TEST(ipv6_address::any() < ipv6_address::loopback());
        BOOST_TEST(ipv6_address::loopback() > ipv6_address::any());
        BOOST_TEST(a1 <= a2);
        BOOST_TEST(a1 >= a2);

        // Lexicographic on the bytes, network order
        ipv6_address::bytes_type lo{};
        lo[15] = 1;
        ipv6_address::bytes_type hi{};
        hi[0] = 1;
        BOOST_TEST(ipv6_address(lo) < ipv6_address(hi));
    }

    void testScopeId()
    {
        ipv6_address::bytes_type ll{};
        ll[0]  = 0xfe;
        ll[1]  = 0x80;
        ll[15] = 1; // fe80::1

        // Default is the unscoped address
        BOOST_TEST_EQ(ipv6_address(ll).scope_id(), 0u);
        BOOST_TEST_EQ(ipv6_address().scope_id(), 0u);
        BOOST_TEST_EQ(ipv6_address::loopback().scope_id(), 0u);

        // Construct with a zone
        ipv6_address a(ll, 2);
        BOOST_TEST_EQ(a.scope_id(), 2u);

        // The mapping constructor yields an unscoped address
        BOOST_TEST_EQ(ipv6_address(ipv4_address::loopback()).scope_id(), 0u);

        // Same bytes on different links are different values
        ipv6_address b(ll, 3);
        ipv6_address c(ll, 2);
        BOOST_TEST(a != b);
        BOOST_TEST(a == c);
        BOOST_TEST(a != ipv6_address(ll)); // scoped != unscoped

        // Zone is the ordering tiebreaker after the bytes
        BOOST_TEST(ipv6_address(ll) < a);
        BOOST_TEST(a < b);
        BOOST_TEST((a <=> c) == std::strong_ordering::equal);
        ipv6_address::bytes_type hi = ll;
        hi[15]                      = 2;
        BOOST_TEST(b < ipv6_address(hi)); // bytes outrank zone

        // Classification tests the address bits, not the link
        ipv6_address::bytes_type lo{};
        lo[15] = 1;
        BOOST_TEST(ipv6_address(lo, 2).is_loopback());
        BOOST_TEST(
            ipv6_address(ipv6_address::bytes_type{}, 2).is_unspecified());
    }

    void testScopeIdText()
    {
        // Numeric zone parses on every platform and round-trips
        {
            auto [ec, a] = make_ipv6_address("fe80::1%2");
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(a.scope_id(), 2u);
            BOOST_TEST_EQ(a.to_string(), "fe80::1%2");
        }

        // Zone 0 prints without a suffix
        BOOST_TEST_EQ(ipv6_address("fe80::1").to_string(), "fe80::1");

        // A zone is accepted on any v6 address, up to the uint32 max
        {
            auto [ec, a] = make_ipv6_address("2001:db8::1%4294967295");
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(a.scope_id(), 4294967295u);
            BOOST_TEST_EQ(a.to_string(), "2001:db8::1%4294967295");
        }

        // Ostream agrees with to_string
        {
            std::ostringstream oss;
            oss << ipv6_address("fe80::1%3");
            BOOST_TEST_EQ(oss.str(), "fe80::1%3");
        }

        // The true longest form (39 address chars + 11 zone chars)
        // fits the documented capacity, pinned numerically so the
        // constant cannot silently shrink below what print_impl emits
        {
            BOOST_TEST_EQ(ipv6_address::max_str_len, 60u);
            char buf[64];
            ipv6_address::bytes_type all_ff;
            all_ff.fill(0xff);
            ipv6_address a(all_ff, 4294967295u);
            auto sv = a.to_buffer(buf, sizeof(buf));
            BOOST_TEST_EQ(
                sv, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295");
            BOOST_TEST(sv.size() <= ipv6_address::max_str_len);

            // The mapped form with a zone stays within bounds too
            ipv6_address m(
                ipv6_address("::ffff:255.255.255.255").to_bytes(), 4294967295u);
            auto sm = m.to_buffer(buf, sizeof(buf));
            BOOST_TEST_EQ(sm, "::ffff:255.255.255.255%4294967295");
            BOOST_TEST(sm.size() <= ipv6_address::max_str_len);
        }

        // Rejections: never a silent zone
        auto rejects = [](std::string_view s) {
            auto [ec, addr] = make_ipv6_address(s);
            if (ec)
                BOOST_TEST(addr == ipv6_address());
            return bool(ec);
        };
        BOOST_TEST(rejects("fe80::1%"));           // empty zone
        BOOST_TEST(rejects("fe80::1%%2"));         // doubled separator
        BOOST_TEST(rejects("fe80::1%4294967296")); // exceeds uint32
        BOOST_TEST(rejects("fe80::1%-1"));         // sign is not a digit
        BOOST_TEST(rejects("%2"));                 // zone without address
        BOOST_TEST(rejects("fe80::1%no/such"));    // impossible name

#if BOOST_COROSIO_POSIX
        // A real interface name maps through if_nametoindex
        char name[IF_NAMESIZE];
        if (if_indextoname(1, name) != nullptr)
        {
            std::string s = "fe80::1%";
            s += name;
            auto [ec, a] = make_ipv6_address(s);
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(a.scope_id(), 1u);
        }
#else
        // Windows accepts numeric zones only
        BOOST_TEST(rejects("fe80::1%eth0"));
#endif
    }

    void testToV4()
    {
        // Unmapping recovers the exact v4 address
        {
            ipv4_address v4(0xC0A80101); // 192.168.1.1
            ipv6_address mapped(v4);
            BOOST_TEST(mapped.is_v4_mapped());
            BOOST_TEST_EQ(mapped.to_v4(), v4);
        }

        // Round-trips through the mapping constructor
        {
            ipv4_address v4 = ipv4_address::broadcast();
            BOOST_TEST_EQ(ipv6_address(v4).to_v4(), v4);
        }

        // Non-mapped addresses refuse the conversion
        BOOST_TEST_THROWS(ipv6_address::loopback().to_v4(), std::system_error);
        try
        {
            ipv6_address().to_v4();
            BOOST_TEST_FAIL();
        }
        catch (std::system_error const& e)
        {
            BOOST_TEST(e.code() == std::errc::address_family_not_supported);
        }
    }

    void testHash()
    {
        // Equal values hash equal; the zone participates
        ipv6_address::bytes_type ll{};
        ll[0]  = 0xfe;
        ll[1]  = 0x80;
        ll[15] = 1;
        BOOST_TEST_EQ(
            std::hash<ipv6_address>()(ipv6_address(ll, 2)),
            std::hash<ipv6_address>()(ipv6_address(ll, 2)));

        // Zoned and unzoned are distinct keys
        std::unordered_set<ipv6_address> set;
        set.insert(ipv6_address(ll));
        set.insert(ipv6_address(ll, 2));
        set.insert(ipv6_address(ll, 3));
        set.insert(ipv6_address(ll, 2));
        BOOST_TEST_EQ(set.size(), 3u);
        BOOST_TEST(set.contains(ipv6_address(ll, 3)));
    }

    void testOstream()
    {
        std::ostringstream oss;
        oss << ipv6_address::loopback();
        BOOST_TEST_EQ(oss.str(), "::1");
    }

    void testParseRejectionArms()
    {
        // Each string lands on a distinct rejection arm of the
        // IPv4-in-IPv6 tail validation.
        auto check_invalid = [](std::string_view sv) {
            auto [ec, addr] = make_ipv6_address(sv);
            BOOST_TEST(ec == std::errc::invalid_argument);
            BOOST_TEST(addr == ipv6_address());
        };
        // decimal reinterpretation exceeds 255
        check_invalid("::256.0.0.0");
        // middle hex nibble is not a decimal digit
        check_invalid("::1a1.2.3.4");
        // low hex nibble is not a decimal digit
        check_invalid("::10a.2.3.4");
        // h16 expected at end of input
        check_invalid("::1:");
        check_invalid("1:2:3:4:5:6:7:");
        // IPv4 tail cut short
        check_invalid("::1.2.3.");
        check_invalid("::1.2.3.4.5");
    }

    void testToBufferBoundaries()
    {
        char exact[ipv6_address::max_str_len];
        auto sv = ipv6_address::loopback().to_buffer(exact, sizeof(exact));
        BOOST_TEST_EQ(sv, "::1");

        char short_by_one[ipv6_address::max_str_len - 1];
        BOOST_TEST_THROWS(
            ipv6_address::loopback().to_buffer(
                short_by_one, sizeof(short_by_one)),
            std::length_error);
        BOOST_TEST_THROWS(
            ipv6_address::loopback().to_buffer(exact, 0), std::length_error);
    }

    void run()
    {
        testConstruction();
        testParse();
        testParseEndsWithDoubleColon();
        testParseInvalidIPv4Suffix();
        testParseMoreEdges();
        testParseRejectionArms();
        testToBufferBoundaries();
        testToString();
        testToStringHexWidths();
        testToBuffer();
        testToBufferTooSmallThrows();
        testPredicates();
        testComparison();
        testOrdering();
        testScopeId();
        testScopeIdText();
        testToV4();
        testHash();
        testOstream();
    }
};

TEST_SUITE(ipv6_address_test, "boost.corosio.ipv6_address");

} // namespace boost::corosio
