//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4q.udp.adoc.

// Fragments deliberately leave results and bindings unused; the pages
// explain the values in prose instead.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wunused-function"
// the page's bind fragment shows `if (ec) /* handle */;` verbatim
#pragma GCC diagnostic ignored "-Wempty-body"
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
#pragma warning(disable: 4390) // empty controlled statement; shown in the page
#endif

// tag::assume[]
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::assume[]

#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>

#include <cstring>
#include <stop_token>
#include <system_error>

#include "test_suite.hpp"

namespace {

void
open_by_family(corosio::io_context& ioc)
{
    // tag::protocol[]
    corosio::udp_socket sock(ioc);
    if (auto ec = sock.open(corosio::udp::v4()))   // SOCK_DGRAM, AF_INET
        return;  // report the error
    // or open(corosio::udp::v6()) for SOCK_DGRAM, AF_INET6
    // end::protocol[]
}

void
open_and_bind(corosio::io_context& ioc)
{
    // tag::open_bind[]
    corosio::udp_socket sock(ioc);
    if (auto ec = sock.open(corosio::udp::v4()))
        return;  // report the error

    if (auto ec = sock.bind(
            corosio::endpoint(corosio::ipv4_address::any(), 9000)))
        return;  // handle bind failure
    // end::open_bind[]
}

capy::task<>
send_datagram(
    corosio::udp_socket& sock, std::error_code& ec_out, std::size_t& n_out)
{
    // tag::send_to[]
    char const msg[] = "hello";
    corosio::endpoint dest(corosio::ipv4_address::loopback(), 9000);

    auto [ec, n] = co_await sock.send_to(
        capy::const_buffer(msg, sizeof(msg)), dest);
    // end::send_to[]
    ec_out = ec;
    n_out = n;
}

capy::task<>
receive_datagram(
    corosio::udp_socket& sock, std::error_code& ec_out,
    std::size_t& n_out, corosio::endpoint& sender_out)
{
    // tag::recv_from[]
    char buf[1500];
    corosio::endpoint sender;

    auto [ec, n] = co_await sock.recv_from(
        capy::mutable_buffer(buf, sizeof(buf)), sender);
    if (!ec)
    {
        // buf[0..n) holds the datagram; sender holds the source address.
    }
    // end::recv_from[]
    ec_out = ec;
    n_out = n;
    sender_out = sender;
}

// tag::echo[]
capy::task<> echo(corosio::io_context& ioc)
{
    corosio::udp_socket sock(ioc);
    if (auto ec = sock.open(corosio::udp::v4()))
        co_return;
    if (auto ec = sock.bind(
            corosio::endpoint(corosio::ipv4_address::any(), 9000)))
        co_return;

    char buf[1500];
    for (;;)
    {
        corosio::endpoint sender;
        auto [rec, n] = co_await sock.recv_from(
            capy::mutable_buffer(buf, sizeof(buf)), sender);
        if (rec) co_return;

        if (auto [sec, sn] = co_await sock.send_to(
                capy::const_buffer(buf, n), sender); sec)
            co_return;
    }
}
// end::echo[]

// The connected-mode fragment expects a live responder on the page's
// literal port; it is compiled but never executed.
[[maybe_unused]] capy::task<>
connected_mode(corosio::io_context& ioc)
{
    // tag::connected[]
    corosio::udp_socket sock(ioc);
    auto [cec] = co_await sock.connect(
        corosio::endpoint(corosio::ipv4_address::loopback(), 9000));
    if (cec) co_return;

    if (auto [sec, sn] = co_await sock.send(
            capy::const_buffer("ping", 4)); sec)
        co_return;

    char buf[64];
    auto [rec, n] = co_await sock.recv(
        capy::mutable_buffer(buf, sizeof(buf)));
    // end::connected[]
}

capy::task<>
peek_datagram(
    corosio::udp_socket& sock, std::size_t& peeked, std::size_t& drained)
{
    char buf[64];
    corosio::endpoint sender;
    // tag::peek[]
    auto [ec, n] = co_await sock.recv_from(
        capy::mutable_buffer(buf, sizeof(buf)), sender,
        corosio::message_flags::peek);
    // end::peek[]
    if (ec)
        co_return;
    peeked = n;

    // The peeked datagram is still queued; a plain recv drains it.
    auto [ec2, n2] = co_await sock.recv_from(
        capy::mutable_buffer(buf, sizeof(buf)), sender);
    if (!ec2)
        drained = n2;
}

bool
tune_options(corosio::udp_socket& sock)
{
    // tag::options[]
    sock.set_option(corosio::socket_option::reuse_address(true));
    sock.set_option(corosio::socket_option::broadcast(true));
    sock.set_option(corosio::socket_option::receive_buffer_size(1 << 20));

    auto bcast = sock.get_option<corosio::socket_option::broadcast>();
    // end::options[]
    return bcast.value();
}

// Joining a multicast group depends on the host's multicast routing
// (it can fail in containers), so this fragment never runs.
[[maybe_unused]] capy::task<>
multicast_join(corosio::io_context& ioc)
{
    // tag::multicast[]
    corosio::udp_socket sock(ioc);
    if (auto ec = sock.open(corosio::udp::v4()))
        co_return;
    sock.set_option(corosio::socket_option::reuse_address(true));

    if (auto ec = sock.bind(
            corosio::endpoint(corosio::ipv4_address::any(), 30001)))
        co_return;

    sock.set_option(corosio::socket_option::join_group_v4(
        corosio::ipv4_address("239.255.0.1")));
    // end::multicast[]
}

void
cancel_all(corosio::udp_socket& sock)
{
    // tag::cancel[]
    sock.cancel();
    // end::cancel[]
}

struct udp_test
{
    void
    testProtocolAndBind()
    {
        corosio::io_context ioc;
        open_by_family(ioc);
        open_and_bind(ioc);
        BOOST_TEST_PASS();
    }

    void
    testSendTo()
    {
        corosio::io_context ioc;
        corosio::udp_socket sock(ioc);
        BOOST_TEST(!sock.open(corosio::udp::v4()));

        std::error_code ec;
        std::size_t n = 0;
        capy::run_async(ioc.get_executor())(send_datagram(sock, ec, n));
        ioc.run();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 6u);
    }

    void
    testRecvFrom()
    {
        corosio::io_context ioc;
        auto ex = ioc.get_executor();

        corosio::udp_socket sock(ioc);
        BOOST_TEST(!sock.open(corosio::udp::v4()));
        auto bec = sock.bind(
            corosio::endpoint(corosio::ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);

        corosio::udp_socket helper(ioc);
        BOOST_TEST(!helper.open(corosio::udp::v4()));

        std::error_code ec;
        std::size_t n = 0;
        corosio::endpoint sender;
        capy::run_async(ex)(receive_datagram(sock, ec, n, sender));

        auto feeder = [&]() -> capy::task<>
        {
            co_await helper.send_to(
                capy::const_buffer("hello", 5), sock.local_endpoint());
        };
        capy::run_async(ex)(feeder());

        ioc.run();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 5u);
    }

    void
    testEchoStops()
    {
        // A pre-signaled stop token makes every path through echo()
        // terminate: bind failure returns, and an in-flight recv_from
        // completes with the canceled error.
        corosio::io_context ioc;
        std::stop_source ss;
        ss.request_stop();
        bool done = false;

        auto track = [](capy::task<> t, bool& d) -> capy::task<>
        {
            co_await std::move(t);
            d = true;
        };
        capy::run_async(ioc.get_executor(), ss.get_token())(
            track(echo(ioc), done));
        ioc.run();
        BOOST_TEST(done);
    }

    void
    testPeek()
    {
        corosio::io_context ioc;
        auto ex = ioc.get_executor();

        corosio::udp_socket sock(ioc);
        BOOST_TEST(!sock.open(corosio::udp::v4()));
        auto bec = sock.bind(
            corosio::endpoint(corosio::ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);

        corosio::udp_socket helper(ioc);
        BOOST_TEST(!helper.open(corosio::udp::v4()));

        std::size_t peeked = 0;
        std::size_t drained = 0;
        capy::run_async(ex)(peek_datagram(sock, peeked, drained));

        auto feeder = [&]() -> capy::task<>
        {
            co_await helper.send_to(
                capy::const_buffer("data", 4), sock.local_endpoint());
        };
        capy::run_async(ex)(feeder());

        ioc.run();
        BOOST_TEST_EQ(peeked, 4u);
        BOOST_TEST_EQ(drained, 4u);
    }

    void
    testOptions()
    {
        corosio::io_context ioc;
        corosio::udp_socket sock(ioc);
        BOOST_TEST(!sock.open(corosio::udp::v4()));
        BOOST_TEST(tune_options(sock));
    }

    void
    testCancel()
    {
        corosio::io_context ioc;
        corosio::udp_socket sock(ioc);
        BOOST_TEST(!sock.open(corosio::udp::v4()));
        cancel_all(sock);
        BOOST_TEST_PASS();
    }

    void
    testStopTokenCancel()
    {
        corosio::io_context ioc;
        std::error_code task_ec;

        auto my_task = [&]() -> capy::task<>
        {
            corosio::udp_socket s(ioc);
            BOOST_TEST(!s.open(corosio::udp::v4()));
            if (auto bec = s.bind(corosio::endpoint(
                    corosio::ipv4_address::loopback(), 0)))
                co_return;
            char buf[64];
            corosio::endpoint sender;
            auto [ec, n] = co_await s.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), sender);
            task_ec = ec;
        };

        // tag::stop_cancel[]
        std::stop_source ss;
        capy::run_async(ioc.get_executor(), ss.get_token())(my_task());
        // ...
        ss.request_stop();   // unblocks any in-flight recv_from inside my_task
        // end::stop_cancel[]

        ioc.run();
        BOOST_TEST(task_ec == capy::cond::canceled);
    }

    void
    run()
    {
        testProtocolAndBind();
        testSendTo();
        testRecvFrom();
        testEchoStops();
#if BOOST_COROSIO_POSIX
        // IOCP delivery order does not guarantee the self-sent
        // datagram is queued before the peek executes.
        testPeek();
#endif
        testOptions();
        testCancel();
        testStopTokenCancel();
    }
};

} // namespace

TEST_SUITE(udp_test, "boost.corosio.doc.4q_udp");
