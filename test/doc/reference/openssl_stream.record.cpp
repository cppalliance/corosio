//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/openssl_stream.hpp's
// documentation for openssl_stream, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what
// the reference renders; scaffolding stays outside the tags.
//
// Guarded on BOOST_COROSIO_HAS_OPENSSL, outside the tag, for the same
// reason wolfssl_stream.record.cpp guards on BOOST_COROSIO_HAS_WOLFSSL --
// see test/doc/CMakeLists.txt's OpenSSL_FOUND block for why.

#include "../doc_warnings.hpp"

#if defined(BOOST_COROSIO_HAS_OPENSSL)
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/tls_context.hpp>
#include <boost/corosio/tls_stream.hpp>

#include <boost/capy/task.hpp>

#include <utility>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

// tag::openssl_stream[]
// Two independently connected sockets demonstrate the two construction
// modes; reusing one socket for both would leave tls pointing at sock
// after it was gutted by the move into tls2 (use-after-move), not a
// dangling reference -- sock itself stays in scope.
capy::task<>
reference_and_owning_construction(
    corosio::io_context& ioc, corosio::endpoint ep)
{
    corosio::tls_context ctx;
    if (auto ec = ctx.set_default_verify_paths()) // trust the system CAs
        co_return;
    if (auto ec = ctx.set_verify_mode(corosio::tls_verify_mode::peer))
        co_return;

    // Reference mode - sock must outlive tls
    corosio::tcp_socket sock(ioc);
    auto [ec] = co_await sock.connect(ep);
    if (ec)
        co_return;
    corosio::openssl_stream tls(&sock, ctx);
    tls.set_hostname("example.com");
    auto [hec] = co_await tls.handshake(corosio::tls_role::client);
    if (hec)
        co_return;

    // Or owning mode - tls2 takes ownership of its own connected socket
    corosio::tcp_socket sock2(ioc);
    auto [ec2] = co_await sock2.connect(ep);
    if (ec2)
        co_return;
    corosio::openssl_stream tls2(std::move(sock2), ctx);
}
// end::openssl_stream[]

} // namespace
#endif // BOOST_COROSIO_HAS_OPENSSL
