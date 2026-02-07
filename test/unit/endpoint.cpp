//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/endpoint.hpp>

#include <string>
#include <system_error>

#include "test_suite.hpp"

namespace boost::corosio {

struct endpoint_parse_test
{
    void
    testConstructFromEndpointAndPort()
    {
        // IPv4 case
        {
            endpoint ep1(ipv4_address::loopback(), 8080);
            endpoint ep2(ep1, 443);
            BOOST_TEST(ep2.is_v4());
            BOOST_TEST_EQ(ep2.v4_address(), ipv4_address::loopback());
            BOOST_TEST_EQ(ep2.port(), 443);
        }

        // IPv6 case
        {
            endpoint ep1(ipv6_address::loopback(), 8080);
            endpoint ep2(ep1, 443);
            BOOST_TEST(ep2.is_v6());
            BOOST_TEST(ep2.v6_address().is_loopback());
            BOOST_TEST_EQ(ep2.port(), 443);
        }
    }

    void
    testConstructFromString()
    {
        // IPv4 without port
        {
            endpoint ep("192.168.1.1");
            BOOST_TEST(ep.is_v4());
            BOOST_TEST_EQ(ep.v4_address().to_string(), "192.168.1.1");
            BOOST_TEST_EQ(ep.port(), 0);
        }

        // IPv4 with port
        {
            endpoint ep("192.168.1.1:8080");
            BOOST_TEST(ep.is_v4());
            BOOST_TEST_EQ(ep.v4_address().to_string(), "192.168.1.1");
            BOOST_TEST_EQ(ep.port(), 8080);
        }

        // IPv4 port edge cases
        {
            endpoint ep1("127.0.0.1:0");
            BOOST_TEST_EQ(ep1.port(), 0);

            endpoint ep2("127.0.0.1:65535");
            BOOST_TEST_EQ(ep2.port(), 65535);
        }

        // IPv6 without port
        {
            endpoint ep("::1");
            BOOST_TEST(ep.is_v6());
            BOOST_TEST(ep.v6_address().is_loopback());
            BOOST_TEST_EQ(ep.port(), 0);
        }

        // IPv6 without port (full form)
        {
            endpoint ep("2001:db8::1");
            BOOST_TEST(ep.is_v6());
            BOOST_TEST_EQ(ep.port(), 0);
        }

        // IPv6 bracketed without port
        {
            endpoint ep("[::1]");
            BOOST_TEST(ep.is_v6());
            BOOST_TEST(ep.v6_address().is_loopback());
            BOOST_TEST_EQ(ep.port(), 0);
        }

        // IPv6 bracketed with port
        {
            endpoint ep("[::1]:8080");
            BOOST_TEST(ep.is_v6());
            BOOST_TEST(ep.v6_address().is_loopback());
            BOOST_TEST_EQ(ep.port(), 8080);
        }

        // IPv6 full address bracketed with port
        {
            endpoint ep("[2001:db8::1]:443");
            BOOST_TEST(ep.is_v6());
            BOOST_TEST_EQ(ep.port(), 443);
        }
    }

    void
    testConstructFromStringThrows()
    {
        // Empty string
        BOOST_TEST_THROWS(endpoint(""), std::system_error);

        // Invalid IPv4
        BOOST_TEST_THROWS(endpoint("256.0.0.1"), std::system_error);
        BOOST_TEST_THROWS(endpoint("1.2.3"), std::system_error);
        BOOST_TEST_THROWS(endpoint("1.2.3.4.5"), std::system_error);

        // Invalid port
        BOOST_TEST_THROWS(endpoint("1.2.3.4:"), std::system_error);
        BOOST_TEST_THROWS(endpoint("1.2.3.4:abc"), std::system_error);
        BOOST_TEST_THROWS(endpoint("1.2.3.4:65536"), std::system_error);
        BOOST_TEST_THROWS(endpoint("1.2.3.4:-1"), std::system_error);
        BOOST_TEST_THROWS(endpoint("1.2.3.4:01"), std::system_error);

        // Invalid IPv6
        BOOST_TEST_THROWS(endpoint("["), std::system_error);
        BOOST_TEST_THROWS(endpoint("[]"), std::system_error);
        BOOST_TEST_THROWS(endpoint("[::1"), std::system_error);
        BOOST_TEST_THROWS(endpoint("[::1]:"), std::system_error);
        BOOST_TEST_THROWS(endpoint("[::1]:abc"), std::system_error);
        BOOST_TEST_THROWS(endpoint("[::1]:65536"), std::system_error);
    }

    void
    testDetectFormat()
    {
        BOOST_TEST(detect_endpoint_format("192.168.1.1") == 
            endpoint_format::ipv4_no_port);
        BOOST_TEST(detect_endpoint_format("192.168.1.1:8080") == 
            endpoint_format::ipv4_with_port);
        BOOST_TEST(detect_endpoint_format("::1") == 
            endpoint_format::ipv6_no_port);
        BOOST_TEST(detect_endpoint_format("2001:db8::1") == 
            endpoint_format::ipv6_no_port);
        BOOST_TEST(detect_endpoint_format("[::1]") == 
            endpoint_format::ipv6_bracketed);
        BOOST_TEST(detect_endpoint_format("[::1]:8080") == 
            endpoint_format::ipv6_bracketed);
    }

    void
    testParseIPv4NoPort()
    {
        endpoint ep;
        auto ec = parse_endpoint("192.168.1.1", ep);
        BOOST_TEST(!ec);
        BOOST_TEST(ep.is_v4());
        BOOST_TEST_EQ(ep.port(), 0);
        BOOST_TEST_EQ(ep.v4_address().to_string(), "192.168.1.1");
    }

    void
    testParseIPv4WithPort()
    {
        endpoint ep;
        auto ec = parse_endpoint("192.168.1.1:8080", ep);
        BOOST_TEST(!ec);
        BOOST_TEST(ep.is_v4());
        BOOST_TEST_EQ(ep.port(), 8080);
        BOOST_TEST_EQ(ep.v4_address().to_string(), "192.168.1.1");

        // Edge cases
        ec = parse_endpoint("127.0.0.1:0", ep);
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(ep.port(), 0);

        ec = parse_endpoint("127.0.0.1:65535", ep);
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(ep.port(), 65535);
    }

    void
    testParseIPv6NoPort()
    {
        endpoint ep;
        auto ec = parse_endpoint("::1", ep);
        BOOST_TEST(!ec);
        BOOST_TEST(ep.is_v6());
        BOOST_TEST_EQ(ep.port(), 0);
        BOOST_TEST(ep.v6_address().is_loopback());

        ec = parse_endpoint("2001:db8::1", ep);
        BOOST_TEST(!ec);
        BOOST_TEST(ep.is_v6());
        BOOST_TEST_EQ(ep.port(), 0);
    }

    void
    testParseIPv6Bracketed()
    {
        endpoint ep;

        // Bracketed without port
        auto ec = parse_endpoint("[::1]", ep);
        BOOST_TEST(!ec);
        BOOST_TEST(ep.is_v6());
        BOOST_TEST_EQ(ep.port(), 0);
        BOOST_TEST(ep.v6_address().is_loopback());

        // Bracketed with port
        ec = parse_endpoint("[::1]:8080", ep);
        BOOST_TEST(!ec);
        BOOST_TEST(ep.is_v6());
        BOOST_TEST_EQ(ep.port(), 8080);
        BOOST_TEST(ep.v6_address().is_loopback());

        // Full address with port
        ec = parse_endpoint("[2001:db8::1]:443", ep);
        BOOST_TEST(!ec);
        BOOST_TEST(ep.is_v6());
        BOOST_TEST_EQ(ep.port(), 443);
    }

    void
    testParseInvalid()
    {
        auto check_invalid = [](std::string_view s)
        {
            endpoint ep;
            auto ec = parse_endpoint(s, ep);
            BOOST_TEST(bool(ec));
        };

        // Empty
        check_invalid("");

        // Invalid IPv4
        check_invalid("256.0.0.1");
        check_invalid("1.2.3");
        check_invalid("1.2.3.4.5");

        // Invalid port
        check_invalid("1.2.3.4:");
        check_invalid("1.2.3.4:abc");
        check_invalid("1.2.3.4:65536");
        check_invalid("1.2.3.4:-1");
        check_invalid("1.2.3.4:01");  // leading zero

        // Invalid IPv6
        check_invalid("[");
        check_invalid("[]");
        check_invalid("[::1");
        check_invalid("[::1]:");
        check_invalid("[::1]:abc");
        check_invalid("[::1]:65536");
    }

    void
    run()
    {
        testConstructFromEndpointAndPort();
        testConstructFromString();
        testConstructFromStringThrows();
        testDetectFormat();
        testParseIPv4NoPort();
        testParseIPv4WithPort();
        testParseIPv6NoPort();
        testParseIPv6Bracketed();
        testParseInvalid();
    }
};

TEST_SUITE(endpoint_parse_test, "boost.corosio.endpoint");

} // namespace boost::corosio
