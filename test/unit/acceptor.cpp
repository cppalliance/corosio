//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/acceptor.hpp>

#include <boost/corosio/io_context.hpp>

#include "test_suite.hpp"

namespace boost {
namespace corosio {

//------------------------------------------------
// Acceptor-specific tests
// Focus: acceptor construction and basic interface
//------------------------------------------------

struct acceptor_test
{
    void
    testConstruction()
    {
        io_context ioc;
        acceptor acc(ioc);

        // Acceptor should not be open initially
        BOOST_TEST_EQ(acc.is_open(), false);
    }

    void
    testListen()
    {
        io_context ioc;
        acceptor acc(ioc);

        // Listen on a port
        acc.listen(endpoint(0));  // Port 0 = ephemeral port
        BOOST_TEST_EQ(acc.is_open(), true);

        // Close it
        acc.close();
        BOOST_TEST_EQ(acc.is_open(), false);
    }

    void
    testMoveConstruct()
    {
        io_context ioc;
        acceptor acc1(ioc);
        acc1.listen(endpoint(0));
        BOOST_TEST_EQ(acc1.is_open(), true);

        // Move construct
        acceptor acc2(std::move(acc1));
        BOOST_TEST_EQ(acc1.is_open(), false);
        BOOST_TEST_EQ(acc2.is_open(), true);

        acc2.close();
    }

    void
    testMoveAssign()
    {
        io_context ioc;
        acceptor acc1(ioc);
        acceptor acc2(ioc);
        acc1.listen(endpoint(0));
        BOOST_TEST_EQ(acc1.is_open(), true);
        BOOST_TEST_EQ(acc2.is_open(), false);

        // Move assign
        acc2 = std::move(acc1);
        BOOST_TEST_EQ(acc1.is_open(), false);
        BOOST_TEST_EQ(acc2.is_open(), true);

        acc2.close();
    }

    void
    run()
    {
        testConstruction();
        testListen();
        testMoveConstruct();
        testMoveAssign();
    }
};

TEST_SUITE(acceptor_test, "boost.corosio.acceptor");

} // namespace corosio
} // namespace boost
