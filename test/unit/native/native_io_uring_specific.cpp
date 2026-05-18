//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "test_suite.hpp"

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IO_URING

#include <boost/corosio/backend.hpp>
#include <boost/corosio/io_context.hpp>

namespace boost::corosio {

/* io_uring-specific test placeholders.

   Most io_uring behaviors (multishot accept queueing, cancel-by-fd, op
   lifecycle) are exercised by the existing backend-templated test suites
   (tcp_acceptor.io_uring, tcp_socket.io_uring, cancel.io_uring, etc.).
   This file is the slot for io_uring-only tests when they're needed —
   currently just a smoke test.

   Future additions when there's a specific behavior to pin:
   - SQ ring backpressure (>256 in-flight ops): current behavior surfaces
     EAGAIN synchronously per spec section 8; needs a deterministic
     fixture before testing.
   - Probe-and-fall-back: requires loading a seccomp filter at process
     start; deferred to test infrastructure work.
*/
struct native_io_uring_specific_test
{
    void testTagAvailable()
    {
        // io_context constructed with the explicit io_uring tag should
        // work on any host where BOOST_COROSIO_HAS_IO_URING is 1.
        io_context ioc(io_uring);
        BOOST_TEST(!ioc.stopped());
    }

    void run()
    {
        testTagAvailable();
    }
};

TEST_SUITE(
    native_io_uring_specific_test,
    "boost.corosio.native.io_uring_specific");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IO_URING
