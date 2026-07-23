//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/quick-start.adoc.

// Fragments deliberately leave results and bindings unused; the pages
// explain the values in prose instead.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-lambda-capture"
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4834) // discarding [[nodiscard]] return value
#pragma warning(disable: 4189) // local variable initialized but not referenced
#pragma warning(disable: 4100) // unreferenced formal parameter
#pragma warning(disable: 4101) // unreferenced local variable
#pragma warning(disable: 4456) // declaration hides previous local declaration
#pragma warning(disable: 4457) // declaration hides function parameter
#pragma warning(disable: 4458) // declaration hides class member
#pragma warning(disable: 4459) // declaration hides global declaration
#endif

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/buffers/make_buffer.hpp>
#include <boost/capy/task.hpp>

#include <system_error>

#include "test_suite.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

// The error-handling fragments read from a socket the page assumes is
// connected; they are compiled but never executed.
[[maybe_unused]] capy::task<>
structured_bindings(corosio::tcp_socket& sock, capy::mutable_buffer buf)
{
    // tag::error_bindings[]
    auto [ec, n] = co_await sock.read_some(buf);
    if (ec)
    {
        // Handle error
    }
    // end::error_bindings[]
}

[[maybe_unused]] capy::task<>
exception_style(corosio::tcp_socket& sock, capy::mutable_buffer buf)
{
    // tag::error_exceptions[]
    auto [ec, n] = co_await sock.read_some(buf);
    if (ec) throw std::system_error(ec);  // Throws if read fails
    // end::error_exceptions[]
}

struct quick_start_test
{
    void
    run()
    {
        // The fragments above are compile-only; instantiating the
        // enclosing coroutines is the test.
        BOOST_TEST(true);
    }
};

} // namespace

TEST_SUITE(quick_start_test, "boost.corosio.doc.quick_start");
