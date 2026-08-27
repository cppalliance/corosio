//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "fault.hpp"
#include "fault_test_utils.hpp"
#include "context.hpp"
#include "test_suite.hpp"
#include "test_utils.hpp"

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <tuple>

#if BOOST_COROSIO_HAS_IOCP

namespace boost::corosio::test::fault {

/* The lookup behind dissociate_from_iocp, which happens once.

   The NtSetInformationFile pointer lives in a function-local static,
   so the module handle it is resolved through is asked for on the
   first release() the process makes and never again. That is why this
   is a suite of its own: iocp_faults releases a socket too, and
   whichever ran first would leave the other testing something it did
   not mean to.

   The suite name sorts before every other fault suite, so a
   single-process run of the whole executable still meets this test
   with the pointer unresolved. Under CTest and b2 each suite is its
   own process and the ordering does not arise.
*/
struct iocp_dissociate_faults
{
    void testReleaseWithoutNtEntryPoint()
    {
        if(!hook_is_live(sys::GetModuleHandleW))
        {
            skip_dead_hook("GetModuleHandleW");
            return;
        }
        io_context ioc(iocp);
        tcp_socket s(ioc);
        BOOST_TEST(!s.open(tcp::v4()));
        native_handle_type h{};
        {
            // ntdll refuses to answer, so there is no entry point to
            // cache and severing the association is never attempted.
            // Armed immediately before the call because nothing else
            // on this thread asks for a module handle in between.
            fault_scope f(sys::GetModuleHandleW, ERROR_MOD_NOT_FOUND);
            h = s.release();
            BOOST_TEST(f.fired());
        }
        // Severing the association is best effort: the caller gets a
        // working socket whether or not it happened.
        BOOST_TEST(!s.is_open());
        BOOST_TEST(native_socket_valid(h));
        close_native_socket(h);

        // The lookup ran once for the process, so a second release
        // asks nothing and still hands back its socket.
        tcp_socket t(ioc);
        BOOST_TEST(!t.open(tcp::v4()));
        auto const h2 = t.release();
        BOOST_TEST(!t.is_open());
        BOOST_TEST(native_socket_valid(h2));
        close_native_socket(h2);
    }

    void run()
    {
        testReleaseWithoutNtEntryPoint();
    }
};

TEST_SUITE(iocp_dissociate_faults, "boost.corosio.fault.dissociate");

} // boost::corosio::test::fault

#endif
