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

        // TODO: Fix range concept - static assert that consuming_buffers models mutable_buffer_sequence
        // static_assert(
        //     capy::mutable_buffer_sequence<consuming_buffers<decltype(bufs)>>,
        //     "consuming_buffers must model mutable_buffer_sequence");

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

        // TODO: Fix range concept - static assert for single buffer
        // static_assert(
        //     capy::mutable_buffer_sequence<consuming_buffers<capy::mutable_buffer>>,
        //     "consuming_buffers must model mutable_buffer_sequence for single buffer");

        std::size_t const size = capy::buffer_size(cb);
        BOOST_TEST_EQ(size, sizeof(buf));
    }

    void
    run()
    {
        testBufferSequenceConcept();
        testSingleBuffer();
    }
};

TEST_SUITE(consuming_buffers_test, "boost.corosio.consuming_buffers");

} // namespace corosio
} // namespace boost
