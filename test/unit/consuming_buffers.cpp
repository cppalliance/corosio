//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/consuming_buffers.hpp>

#include <boost/capy/buffers.hpp>

#include <array>
#include <concepts>
#include <iterator>
#include <ranges>

#include "test_suite.hpp"

namespace boost {
namespace corosio {

//------------------------------------------------
// consuming_buffers tests
// Focus: verify consuming_buffers models buffer sequence concept
//------------------------------------------------

struct consuming_buffers_test
{
    void
    testBufferSequenceConcept()
    {
        char buf1[100];
        char buf2[200];
        std::array<capy::mutable_buffer, 2> bufs = {
            capy::mutable_buffer(buf1, sizeof(buf1)),
            capy::mutable_buffer(buf2, sizeof(buf2))
        };

        consuming_buffers<decltype(bufs)> cb(bufs);

        // Verify consuming_buffers models mutable_buffer_sequence
        static_assert(
            capy::MutableBufferSequence<consuming_buffers<decltype(bufs)>>,
            "consuming_buffers must model mutable_buffer_sequence");

        // Verify it can be used with buffer_size
        std::size_t const size = capy::buffer_size(cb);
        BOOST_TEST_EQ(size, sizeof(buf1) + sizeof(buf2));
    }

    void
    testSingleBuffer()
    {
        char buf[100];
        capy::mutable_buffer mbuf(buf, sizeof(buf));

        consuming_buffers<capy::mutable_buffer> cb(mbuf);

        // Verify consuming_buffers models mutable_buffer_sequence for single buffer
        static_assert(
            capy::MutableBufferSequence<consuming_buffers<capy::mutable_buffer>>,
            "consuming_buffers must model mutable_buffer_sequence for single buffer");

        std::size_t const size = capy::buffer_size(cb);
        BOOST_TEST_EQ(size, sizeof(buf));
    }

    void
    testRangeConcepts()
    {
        char buf1[100];
        char buf2[200];
        std::array<capy::mutable_buffer, 2> bufs = {
            capy::mutable_buffer(buf1, sizeof(buf1)),
            capy::mutable_buffer(buf2, sizeof(buf2))
        };

        using cb_type = consuming_buffers<decltype(bufs)>;

        // Most general to most specific - Range Concepts
        static_assert(std::ranges::range<cb_type>,
            "consuming_buffers must satisfy std::ranges::range");
        static_assert(std::ranges::input_range<cb_type>,
            "consuming_buffers must satisfy std::ranges::input_range");
        static_assert(std::ranges::forward_range<cb_type>,
            "consuming_buffers must satisfy std::ranges::forward_range");
        static_assert(std::ranges::bidirectional_range<cb_type>,
            "consuming_buffers must satisfy std::ranges::bidirectional_range");

        // Most general to most specific - Iterator Concepts
        using iter_t = std::ranges::iterator_t<cb_type>;
        static_assert(std::input_iterator<iter_t>,
            "consuming_buffers iterator must satisfy std::input_iterator");
        static_assert(std::forward_iterator<iter_t>,
            "consuming_buffers iterator must satisfy std::forward_iterator");
        static_assert(std::bidirectional_iterator<iter_t>,
            "consuming_buffers iterator must satisfy std::bidirectional_iterator");

        // Iterator traits check
        using traits = std::iterator_traits<iter_t>;
        static_assert(std::same_as<typename traits::iterator_category, std::bidirectional_iterator_tag>,
            "Iterator category must be bidirectional_iterator_tag");

        // Range value type check
        static_assert(std::is_convertible_v<std::ranges::range_value_t<cb_type>, capy::mutable_buffer>,
            "Range value type must be convertible to mutable_buffer");

        // Verify std::ranges::begin and std::ranges::end work
        {
            cb_type cb(bufs);
            auto it1 = std::ranges::begin(cb);
            auto it2 = std::ranges::end(cb);
            BOOST_TEST(it1 != it2);
        }

        // Final check - Buffer Sequence Concept
        static_assert(capy::MutableBufferSequence<cb_type>,
            "consuming_buffers must model mutable_buffer_sequence");
    }

    void
    run()
    {
        testBufferSequenceConcept();
        testSingleBuffer();
        testRangeConcepts();
    }
};

TEST_SUITE(consuming_buffers_test, "boost.corosio.consuming_buffers");

} // namespace corosio
} // namespace boost
