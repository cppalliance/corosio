//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/ipv4_address.hpp>

#include <sstream>

#include "test_suite.hpp"

namespace boost::corosio {

struct ipv4_address_test
{
    void testConstruction()
    {
        // Default construction
        {
            ipv4_address a;
            BOOST_TEST(a.is_unspecified());
            BOOST_TEST_EQ(a.to_uint(), 0u);
        }

        // Construct from uint
        {
            ipv4_address a(0x01020304);
            BOOST_TEST_EQ(a.to_uint(), 0x01020304u);
            BOOST_TEST_EQ(a.to_string(), "1.2.3.4");
        }

        // Construct from bytes
        {
            ipv4_address::bytes_type bytes{{192, 168, 1, 1}};
            ipv4_address a(bytes);
            BOOST_TEST_EQ(a.to_string(), "192.168.1.1");
        }

        // Construct from string
        {
            ipv4_address a("10.0.0.1");
            BOOST_TEST_EQ(a.to_string(), "10.0.0.1");
        }

        // Invalid string throws
        {
            BOOST_TEST_THROWS(ipv4_address("invalid"), std::invalid_argument);
            BOOST_TEST_THROWS(ipv4_address("256.0.0.1"), std::invalid_argument);
            BOOST_TEST_THROWS(ipv4_address("1.2.3"), std::invalid_argument);
        }
    }

    void testParse()
    {
        // Valid addresses
        auto check_valid = [](std::string_view s, std::uint32_t expected) {
            ipv4_address addr;
            auto ec = parse_ipv4_address(s, addr);
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(addr.to_uint(), expected);
        };

        check_valid("0.0.0.0", 0x00000000);
        check_valid("1.2.3.4", 0x01020304);
        check_valid("192.168.1.1", 0xC0A80101);
        check_valid("255.255.255.255", 0xFFFFFFFF);
        check_valid("127.0.0.1", 0x7F000001);

        // Invalid addresses
        auto check_invalid = [](std::string_view s) {
            ipv4_address addr;
            auto ec = parse_ipv4_address(s, addr);
            BOOST_TEST(bool(ec));
        };

        check_invalid("");
        check_invalid(".");
        check_invalid("1");
        check_invalid("1.2");
        check_invalid("1.2.3");
        check_invalid("1.2.3.");
        check_invalid("1.2.3.4.");
        check_invalid(".1.2.3.4");
        check_invalid("1..2.3.4");
        check_invalid("256.0.0.0");
        check_invalid("0.256.0.0");
        check_invalid("0.0.256.0");
        check_invalid("0.0.0.256");
        check_invalid("00.0.0.0");   // leading zero
        check_invalid("01.0.0.0");   // leading zero
        check_invalid("1.02.3.4");   // leading zero
        check_invalid("1.2.3.4a");   // trailing garbage
        check_invalid("a1.2.3.4");   // leading garbage
        check_invalid("1.2.3.4.5");  // too many octets
        check_invalid("-1.2.3.4");   // negative
        check_invalid("1000.2.3.4"); // too large
    }

    void testToBytes()
    {
        ipv4_address a(0x01020304);
        auto bytes = a.to_bytes();
        BOOST_TEST_EQ(bytes[0], 1);
        BOOST_TEST_EQ(bytes[1], 2);
        BOOST_TEST_EQ(bytes[2], 3);
        BOOST_TEST_EQ(bytes[3], 4);
    }

    void testToString()
    {
        BOOST_TEST_EQ(ipv4_address(0x00000000).to_string(), "0.0.0.0");
        BOOST_TEST_EQ(ipv4_address(0x01020304).to_string(), "1.2.3.4");
        BOOST_TEST_EQ(ipv4_address(0xFFFFFFFF).to_string(), "255.255.255.255");
        BOOST_TEST_EQ(ipv4_address(0x0A0B0C0D).to_string(), "10.11.12.13");
    }

    void testToBuffer()
    {
        char buf[ipv4_address::max_str_len];
        auto sv = ipv4_address(0x01020304).to_buffer(buf, sizeof(buf));
        BOOST_TEST_EQ(sv, "1.2.3.4");
    }

    void testPredicates()
    {
        // Loopback
        BOOST_TEST(ipv4_address(0x7F000001).is_loopback());
        BOOST_TEST(ipv4_address(0x7F000000).is_loopback());
        BOOST_TEST(ipv4_address(0x7FFFFFFF).is_loopback());
        BOOST_TEST(!ipv4_address(0x80000000).is_loopback());

        // Unspecified
        BOOST_TEST(ipv4_address(0x00000000).is_unspecified());
        BOOST_TEST(!ipv4_address(0x00000001).is_unspecified());

        // Multicast (224.0.0.0 - 239.255.255.255)
        BOOST_TEST(ipv4_address(0xE0000000).is_multicast());
        BOOST_TEST(ipv4_address(0xEFFFFFFF).is_multicast());
        BOOST_TEST(!ipv4_address(0xDFFFFFFF).is_multicast());
        BOOST_TEST(!ipv4_address(0xF0000000).is_multicast());
    }

    void testStaticFactories()
    {
        BOOST_TEST(ipv4_address::any().is_unspecified());
        BOOST_TEST_EQ(ipv4_address::any().to_uint(), 0u);

        BOOST_TEST(ipv4_address::loopback().is_loopback());
        BOOST_TEST_EQ(ipv4_address::loopback().to_uint(), 0x7F000001u);

        BOOST_TEST_EQ(ipv4_address::broadcast().to_uint(), 0xFFFFFFFFu);
    }

    void testComparison()
    {
        ipv4_address a1(0x01020304);
        ipv4_address a2(0x01020304);
        ipv4_address a3(0x04030201);

        BOOST_TEST(a1 == a2);
        BOOST_TEST(!(a1 != a2));
        BOOST_TEST(a1 != a3);
        BOOST_TEST(!(a1 == a3));
    }

    void testOstream()
    {
        std::ostringstream oss;
        oss << ipv4_address(0xC0A80101);
        BOOST_TEST_EQ(oss.str(), "192.168.1.1");
    }

    void run()
    {
        testConstruction();
        testParse();
        testToBytes();
        testToString();
        testToBuffer();
        testPredicates();
        testStaticFactories();
        testComparison();
        testOstream();
    }
};

TEST_SUITE(ipv4_address_test, "boost.corosio.ipv4_address");

} // namespace boost::corosio
