//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_STREAM_TESTS_HPP
#define BOOST_COROSIO_TEST_STREAM_TESTS_HPP

#include <boost/capy/concept/stream.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>

#include "test_suite.hpp"

#include <cstring>
#include <string_view>

namespace boost::corosio::test {

/** Test bidirectional data transfer between two streams.

    Writes test data from stream a to b, and from b to a,
    verifying the data is transferred correctly.

    @tparam S1 Stream type satisfying `capy::Stream`
    @tparam S2 Stream type satisfying `capy::Stream`

    @param a First stream (write then read)
    @param b Second stream (read then write)
    @param test_data Data to transfer
*/
template<capy::Stream S1, capy::Stream S2>
capy::task<>
test_echo(S1& a, S2& b, std::string_view test_data = "hello")
{
    std::vector<char> buf(test_data.size() + 16);

    // Write from a, read from b
    {
        auto [ec1, n1] = co_await a.write_some(
            capy::const_buffer(test_data.data(), test_data.size()));
        BOOST_TEST(!ec1);
        if(ec1)
            co_return;
        BOOST_TEST_EQ(n1, test_data.size());

        auto [ec2, n2] = co_await b.read_some(
            capy::mutable_buffer(buf.data(), buf.size()));
        BOOST_TEST(!ec2);
        if(ec2)
            co_return;
        BOOST_TEST_EQ(n2, test_data.size());
        BOOST_TEST(std::memcmp(buf.data(), test_data.data(), n2) == 0);
    }

    // Write from b, read from a
    {
        auto [ec3, n3] = co_await b.write_some(
            capy::const_buffer(test_data.data(), test_data.size()));
        BOOST_TEST(!ec3);
        if(ec3)
            co_return;
        BOOST_TEST_EQ(n3, test_data.size());

        auto [ec4, n4] = co_await a.read_some(
            capy::mutable_buffer(buf.data(), buf.size()));
        BOOST_TEST(!ec4);
        if(ec4)
            co_return;
        BOOST_TEST_EQ(n4, test_data.size());
        BOOST_TEST(std::memcmp(buf.data(), test_data.data(), n4) == 0);
    }
}

/** Generate test data scaled by max_size.

    Returns smaller test data for smaller max_size values
    to keep tests fast while still exercising chunked I/O.

    @param max_size Maximum bytes per I/O operation
    @return Appropriate test data string
*/
inline std::string
scaled_test_data(std::size_t max_size)
{
    if(max_size <= 1)
        return "Hello World!1234";  // 16 bytes
    if(max_size <= 13)
        return std::string(64, 'X');  // 64 bytes
    if(max_size <= 64)
        return std::string(256, 'Y');  // 256 bytes
    return std::string(1024, 'Z');  // 1KB
}

} // namespace boost::corosio::test

#endif
