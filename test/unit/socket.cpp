//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/socket.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/concept/read_stream.hpp>
#include <boost/capy/concept/write_stream.hpp>

#include "test_suite.hpp"

namespace boost {
namespace corosio {

//------------------------------------------------
// Verify socket satisfies stream concepts
//------------------------------------------------

static_assert(capy::ReadStream<socket, capy::mutable_buffer>);
static_assert(capy::WriteStream<socket, capy::const_buffer>);

//------------------------------------------------
// Socket-specific tests
// Focus: socket construction and basic interface
//------------------------------------------------

struct socket_test
{
    void
    testConstruction()
    {
        io_context ioc;
        socket sock(ioc);

        // Socket should not be open initially
        BOOST_TEST_EQ(sock.is_open(), false);
    }

    void
    testOpen()
    {
        io_context ioc;
        socket sock(ioc);

        // Open the socket
        sock.open();
        BOOST_TEST_EQ(sock.is_open(), true);

        // Close it
        sock.close();
        BOOST_TEST_EQ(sock.is_open(), false);
    }

    void
    testMoveConstruct()
    {
        io_context ioc;
        socket sock1(ioc);
        sock1.open();
        BOOST_TEST_EQ(sock1.is_open(), true);

        // Move construct
        socket sock2(std::move(sock1));
        BOOST_TEST_EQ(sock1.is_open(), false);
        BOOST_TEST_EQ(sock2.is_open(), true);

        sock2.close();
    }

    void
    testMoveAssign()
    {
        io_context ioc;
        socket sock1(ioc);
        socket sock2(ioc);
        sock1.open();
        BOOST_TEST_EQ(sock1.is_open(), true);
        BOOST_TEST_EQ(sock2.is_open(), false);

        // Move assign
        sock2 = std::move(sock1);
        BOOST_TEST_EQ(sock1.is_open(), false);
        BOOST_TEST_EQ(sock2.is_open(), true);

        sock2.close();
    }

    void
    run()
    {
        testConstruction();
        testOpen();
        testMoveConstruct();
        testMoveAssign();
    }
};

TEST_SUITE(socket_test, "boost.corosio.socket");

} // namespace corosio
} // namespace boost
