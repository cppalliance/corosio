//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/host_name.hpp>

#include <string>

#include "test_suite.hpp"

namespace boost::corosio {

struct host_name_test
{
    // Every configured machine has a hostname.
    void testReturnsNonEmpty()
    {
        std::string h = host_name();
        BOOST_TEST(!h.empty());
    }

    // Catches buffer or string-lifetime bugs across calls.
    void testStable()
    {
        std::string a = host_name();
        std::string b = host_name();
        BOOST_TEST_EQ(a, b);
    }

    // 255 is the DNS hostname ceiling; anything longer is garbage
    // from a miscounted buffer.
    void testReasonableLength()
    {
        std::string h = host_name();
        BOOST_TEST(h.size() > 0);
        BOOST_TEST(h.size() <= 255);
    }

    // Regression guard for the Windows implementation choice: a
    // switch to winsock gethostname() would fail here because
    // corosio's WSAStartup is lazy (inside io_context).
    void testNoIoContextNeeded()
    {
        std::string h = host_name();
        BOOST_TEST(!h.empty());
    }

    // Catches encoding regressions, especially the Windows
    // UTF-16 -> UTF-8 conversion. Non-ASCII hostnames are valid, so
    // accept any printable ASCII byte or high-bit byte.
    void testCharsetSanity()
    {
        std::string h = host_name();
        for (unsigned char c : h)
        {
            bool printable_ascii = (c >= 0x20 && c <= 0x7E);
            bool high_bit = (c >= 0x80);
            BOOST_TEST(printable_ascii || high_bit);
        }
    }

    void run()
    {
        testReturnsNonEmpty();
        testStable();
        testReasonableLength();
        testNoIoContextNeeded();
        testCharsetSanity();
    }
};

TEST_SUITE(host_name_test, "boost.corosio.host_name");

} // namespace boost::corosio
