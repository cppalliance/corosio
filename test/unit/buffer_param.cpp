//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/detail/buffer_param.hpp>


#include <span>
#include <array>

#include "test_suite.hpp"

namespace boost::corosio {

struct buffer_param_test
{
    // Helper to reduce repeated copy_to assertion pattern
    static void check_copy(
        buffer_param p,
        std::initializer_list<std::pair<void const*, std::size_t>> expected)
    {
        capy::mutable_buffer dest[8];
        auto n = p.copy_to(dest, 8);
        BOOST_TEST_EQ(n, expected.size());
        std::size_t i = 0;
        for (auto const& e : expected)
        {
            BOOST_TEST_EQ(dest[i].data(), e.first);
            BOOST_TEST_EQ(dest[i].size(), e.second);
            ++i;
        }
    }

    // Helper for checking empty/zero-byte sequences
    static void check_empty(buffer_param p)
    {
        capy::mutable_buffer dest[8];
        BOOST_TEST_EQ(p.copy_to(dest, 8), 0);
    }

    void testConstBuffer()
    {
        char const data[] = "Hello";
        capy::const_buffer cb(data, 5);
        check_copy(cb, {{data, 5}});
    }

    void testMutableBuffer()
    {
        char data[] = "Hello";
        capy::mutable_buffer mb(data, 5);
        check_copy(mb, {{data, 5}});
    }

    void testConstBufferPair()
    {
        char const data1[] = "Hello";
        char const data2[] = "World";
        std::array<capy::const_buffer, 2> cbp{
            {capy::const_buffer(data1, 5), capy::const_buffer(data2, 5)}};
        check_copy(cbp, {{data1, 5}, {data2, 5}});
    }

    void testMutableBufferPair()
    {
        char data1[] = "Hello";
        char data2[] = "World";
        std::array<capy::mutable_buffer, 2> mbp{
            {capy::mutable_buffer(data1, 5), capy::mutable_buffer(data2, 5)}};
        check_copy(mbp, {{data1, 5}, {data2, 5}});
    }

    void testSpan()
    {
        char const data1[]        = "One";
        char const data2[]        = "Two";
        char const data3[]        = "Three";
        capy::const_buffer arr[3] = {
            capy::const_buffer(data1, 3), capy::const_buffer(data2, 3),
            capy::const_buffer(data3, 5)};
        std::span<capy::const_buffer const> s(arr, 3);
        check_copy(s, {{data1, 3}, {data2, 3}, {data3, 5}});
    }

    void testArray()
    {
        char const data1[] = "One";
        char const data2[] = "Two";
        char const data3[] = "Three";
        std::array<capy::const_buffer, 3> arr{
            {capy::const_buffer(data1, 3), capy::const_buffer(data2, 3),
             capy::const_buffer(data3, 5)}};
        check_copy(arr, {{data1, 3}, {data2, 3}, {data3, 5}});
    }

    void testCArray()
    {
        char const data1[]        = "One";
        char const data2[]        = "Two";
        char const data3[]        = "Three";
        capy::const_buffer arr[3] = {
            capy::const_buffer(data1, 3), capy::const_buffer(data2, 3),
            capy::const_buffer(data3, 5)};
        check_copy(arr, {{data1, 3}, {data2, 3}, {data3, 5}});
    }

    void testLimitedCopy()
    {
        char const data1[]        = "One";
        char const data2[]        = "Two";
        char const data3[]        = "Three";
        capy::const_buffer arr[3] = {
            capy::const_buffer(data1, 3), capy::const_buffer(data2, 3),
            capy::const_buffer(data3, 5)};

        buffer_param ref(arr);

        // copy only 2 buffers
        capy::mutable_buffer dest[2];
        auto n = ref.copy_to(dest, 2);
        BOOST_TEST_EQ(n, 2);
        BOOST_TEST_EQ(dest[0].data(), data1);
        BOOST_TEST_EQ(dest[0].size(), 3);
        BOOST_TEST_EQ(dest[1].data(), data2);
        BOOST_TEST_EQ(dest[1].size(), 3);
    }

    void testEmptySequence()
    {
        // Zero total bytes returns 0, regardless of buffer count
        capy::const_buffer cb;
        check_empty(cb);
    }

    void testZeroByteConstBuffer()
    {
        // Explicit zero-byte const buffer
        char const* data = "Hello";
        capy::const_buffer cb(data, 0);
        check_empty(cb);
    }

    void testZeroByteMultiple()
    {
        // Multiple zero-byte buffers should still return 0
        char const data1[]        = "Hello";
        char const data2[]        = "World";
        capy::const_buffer arr[3] = {
            capy::const_buffer(data1, 0), capy::const_buffer(data2, 0),
            capy::const_buffer(nullptr, 0)};
        check_empty(arr);
    }

    void testZeroByteBufferPair()
    {
        // Buffer pair with both zero-byte buffers
        char const data1[] = "Hello";
        char const data2[] = "World";
        std::array<capy::const_buffer, 2> cbp{
            {capy::const_buffer(data1, 0), capy::const_buffer(data2, 0)}};
        check_empty(cbp);
    }

    void testMixedZeroAndNonZero()
    {
        // Mix of zero-byte and non-zero buffers
        // Zero-size buffers are skipped, only non-zero returned
        char const data1[]        = "Hello";
        char const data2[]        = "World";
        capy::const_buffer arr[3] = {
            capy::const_buffer(data1, 0), capy::const_buffer(data2, 5),
            capy::const_buffer(nullptr, 0)};
        check_copy(arr, {{data2, 5}});
    }

    void testOneZeroOneNonZero()
    {
        // Buffer pair with one zero-byte, one non-zero
        // Zero-size buffer is skipped
        char const data1[] = "Hello";
        char const data2[] = "World";
        std::array<capy::const_buffer, 2> cbp{
            {capy::const_buffer(data1, 0), capy::const_buffer(data2, 5)}};
        check_copy(cbp, {{data2, 5}});
    }

    void testZeroByteMutableBuffer()
    {
        // Zero-byte mutable buffer
        char data[] = "Hello";
        capy::mutable_buffer mb(data, 0);
        check_empty(mb);
    }

    void testZeroByteMutableBufferPair()
    {
        // Mutable buffer pair with zero-byte buffers
        char data1[] = "Hello";
        char data2[] = "World";
        std::array<capy::mutable_buffer, 2> mbp{
            {capy::mutable_buffer(data1, 0), capy::mutable_buffer(data2, 0)}};
        check_empty(mbp);
    }

    void testEmptySpan()
    {
        // Empty span (no buffers at all)
        std::span<capy::const_buffer const> s;
        check_empty(s);
    }

    void testEmptyArray()
    {
        // Empty std::array (zero-size)
        std::array<capy::const_buffer, 0> arr{};
        check_empty(arr);
    }

    // Helper function that accepts buffer_param by value
    static std::size_t acceptByValue(buffer_param p)
    {
        capy::mutable_buffer dest[8];
        return p.copy_to(dest, 8);
    }

    // Helper function that accepts buffer_param by const reference
    static std::size_t acceptByConstRef(buffer_param const& p)
    {
        capy::mutable_buffer dest[8];
        return p.copy_to(dest, 8);
    }

    void testPassByValue()
    {
        // Test that buffer_param works when passed by value
        char const data[] = "Hello";
        capy::const_buffer cb(data, 5);

        // Pass buffer directly (implicit conversion)
        auto n = acceptByValue(cb);
        BOOST_TEST_EQ(n, 1);

        // Pass buffer_param object
        buffer_param p(cb);
        n = acceptByValue(p);
        BOOST_TEST_EQ(n, 1);

        // Pass buffer sequence directly
        std::array<capy::const_buffer, 2> arr{
            {capy::const_buffer(data, 2), capy::const_buffer(data + 2, 3)}};
        n = acceptByValue(arr);
        BOOST_TEST_EQ(n, 2);
    }

    void testPassByConstRef()
    {
        // Test that buffer_param works when passed by const reference
        char const data[] = "Hello";
        capy::const_buffer cb(data, 5);

        // Pass buffer_param object by const ref
        buffer_param p(cb);
        auto n = acceptByConstRef(p);
        BOOST_TEST_EQ(n, 1);

        // Pass buffer sequence directly (creates temporary buffer_param)
        n = acceptByConstRef(
            std::array<capy::const_buffer, 2>{
                {capy::const_buffer(data, 2),
                 capy::const_buffer(data + 2, 3)}});
        BOOST_TEST_EQ(n, 2);
    }

    void run()
    {
        testConstBuffer();
        testMutableBuffer();
        testConstBufferPair();
        testMutableBufferPair();
        testSpan();
        testArray();
        testCArray();
        testLimitedCopy();
        testEmptySequence();
        testZeroByteConstBuffer();
        testZeroByteMultiple();
        testZeroByteBufferPair();
        testMixedZeroAndNonZero();
        testOneZeroOneNonZero();
        testZeroByteMutableBuffer();
        testZeroByteMutableBufferPair();
        testEmptySpan();
        testEmptyArray();
        testPassByValue();
        testPassByConstRef();
    }
};

TEST_SUITE(buffer_param_test, "boost.corosio.buffer_param");

} // namespace boost::corosio
