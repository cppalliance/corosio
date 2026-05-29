//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/native/native_resolver.hpp>
#include <boost/corosio/native/native_io_context.hpp>

#include <boost/corosio/detail/platform.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#if BOOST_COROSIO_POSIX
#include <netdb.h>
#endif

#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_resolver_test
{
    // resolve(host, service) - forward resolution
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_resolver<Backend>&>().resolve(
                std::declval<std::string_view>(),
                std::declval<std::string_view>())),
            decltype(std::declval<resolver&>().resolve(
                std::declval<std::string_view>(),
                std::declval<std::string_view>()))>,
        "native_resolver::resolve(host, service) must shadow resolver::resolve");
    // resolve(endpoint) - reverse resolution
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_resolver<Backend>&>().resolve(
                std::declval<endpoint const&>())),
            decltype(std::declval<resolver&>().resolve(
                std::declval<endpoint const&>()))>,
        "native_resolver::resolve(endpoint) must shadow resolver::resolve");

    void testResolverConstruct()
    {
        io_context ctx(Backend);
        native_resolver<Backend> r(ctx);
        BOOST_TEST_PASS();
    }

    void testResolverResolve()
    {
        io_context ctx(Backend);
        native_resolver<Backend> r(ctx);

        bool done = false;
        std::error_code result_ec;

        auto task = [](native_resolver<Backend>& r_ref, std::error_code& ec_out,
                       bool& done_out) -> capy::task<> {
            auto [ec, results] = co_await r_ref.resolve("localhost", "80");
            ec_out             = ec;
            done_out           = true;
        };
        capy::run_async(ctx.get_executor())(task(r, result_ec, done));

        ctx.run();
        BOOST_TEST(done);
        BOOST_TEST(!result_ec);
    }

    void testResolverPolymorphicSlice()
    {
        io_context ctx(Backend);
        native_resolver<Backend> nr(ctx);

        resolver& base = nr;
        (void)base;
        BOOST_TEST_PASS();
    }

    void run()
    {
        testResolverConstruct();
        testResolverResolve();
        testResolverPolymorphicSlice();
    }
};

#if BOOST_COROSIO_POSIX
// Direct coverage of make_gai_error switch arms. Real getaddrinfo()
// implementations rarely return anything but EAI_NONAME for synthetic
// inputs, so we drive the helper directly.
struct posix_resolver_gai_error_test
{
    static void check_mapped(int gai_err, std::errc expected)
    {
        auto ec = detail::posix_resolver_detail::make_gai_error(gai_err);
        BOOST_TEST(ec == expected);
    }

    void testEachArm()
    {
        check_mapped(EAI_AGAIN, std::errc::resource_unavailable_try_again);
        check_mapped(EAI_BADFLAGS, std::errc::invalid_argument);
        check_mapped(EAI_FAIL, std::errc::io_error);
        check_mapped(EAI_FAMILY, std::errc::address_family_not_supported);
        check_mapped(EAI_MEMORY, std::errc::not_enough_memory);
        check_mapped(EAI_SERVICE, std::errc::invalid_argument);
        check_mapped(EAI_SOCKTYPE, std::errc::not_supported);

        // EAI_SYSTEM reads errno; force a known value first.
        errno = EINVAL;
        auto sys_ec = detail::posix_resolver_detail::make_gai_error(EAI_SYSTEM);
        BOOST_TEST(sys_ec == std::errc::invalid_argument);

        // Default arm: unknown gai value falls through to io_error.
        check_mapped(9999, std::errc::io_error);
    }

    void run()
    {
        testEachArm();
    }
};

TEST_SUITE(
    posix_resolver_gai_error_test,
    "boost.corosio.native.resolver.posix.make_gai_error");
#endif

#if BOOST_COROSIO_HAS_EPOLL
struct native_resolver_test_epoll : native_resolver_test<epoll>
{};
TEST_SUITE(native_resolver_test_epoll, "boost.corosio.native.resolver.epoll");
#endif

#if BOOST_COROSIO_HAS_SELECT
struct native_resolver_test_select : native_resolver_test<select>
{};
TEST_SUITE(native_resolver_test_select, "boost.corosio.native.resolver.select");
#endif

#if BOOST_COROSIO_HAS_KQUEUE
struct native_resolver_test_kqueue : native_resolver_test<kqueue>
{};
TEST_SUITE(native_resolver_test_kqueue, "boost.corosio.native.resolver.kqueue");
#endif

#if BOOST_COROSIO_HAS_IOCP
struct native_resolver_test_iocp : native_resolver_test<iocp>
{};
TEST_SUITE(native_resolver_test_iocp, "boost.corosio.native.resolver.iocp");
#endif

} // namespace boost::corosio
