//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/native/detail/endpoint_convert.hpp>

#include "test_suite.hpp"

namespace boost::corosio::detail {

struct endpoint_convert_test
{
    void testScopeIdRoundTrip()
    {
        ipv6_address::bytes_type ll{};
        ll[0]  = 0xfe;
        ll[1]  = 0x80;
        ll[15] = 1; // fe80::1

        // The zone reaches the wire form
        {
            endpoint ep(ipv6_address(ll, 7), 8080);
            auto sa = to_sockaddr_in6(ep);
            BOOST_TEST_EQ(sa.sin6_scope_id, 7u);
        }

        // Unscoped stays zero
        {
            endpoint ep(ipv6_address(ll), 8080);
            auto sa = to_sockaddr_in6(ep);
            BOOST_TEST_EQ(sa.sin6_scope_id, 0u);
        }

        // The kernel's zone survives the reverse conversion
        {
            endpoint ep(ipv6_address(ll, 7), 8080);
            auto sa        = to_sockaddr_in6(ep);
            auto recovered = from_sockaddr_in6(sa);
            BOOST_TEST_EQ(recovered.address().to_v6().scope_id(), 7u);
            BOOST_TEST(recovered == ep);
        }

        // Through the storage-based dispatchers as well
        {
            endpoint ep(ipv6_address(ll, 9), 443);
            sockaddr_storage storage;
            auto len = to_sockaddr(ep, storage);
            BOOST_TEST_EQ(len, sizeof(sockaddr_in6));
            auto recovered = from_sockaddr(storage);
            BOOST_TEST(recovered == ep);
        }
    }

    void run()
    {
        testScopeIdRoundTrip();
    }
};

TEST_SUITE(endpoint_convert_test, "boost.corosio.endpoint_convert");

} // namespace boost::corosio::detail
