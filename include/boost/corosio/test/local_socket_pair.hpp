//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_LOCAL_SOCKET_PAIR_HPP
#define BOOST_COROSIO_TEST_LOCAL_SOCKET_PAIR_HPP

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstdio>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace boost::corosio::test {

/** Create a connected pair of AF_UNIX stream sockets via bind+accept+connect.

    Unlike the library-side @ref make_local_stream_pair (POSIX-only,
    socketpair-based), this helper drives the public acceptor API so
    it can produce native template wrappers like
    `native_local_stream_socket<Backend>` — the path benchmarks need
    to exercise the shadowed read_some/write_some/connect ops.

    @tparam Socket   Concrete or native local stream socket type.
    @tparam Acceptor Matching acceptor type.

    @param ctx I/O context backing both sockets.

    @return Connected pair `{accepted, connected}`.
*/
template<
    class Socket   = local_stream_socket,
    class Acceptor = local_stream_acceptor>
std::pair<Socket, Socket>
make_local_stream_pair(io_context& ctx)
{
    namespace fs = std::filesystem;

    static std::random_device rd;
    static std::mt19937_64    gen{rd()};

    std::string path;
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        std::string name = "co_pair_";
        name += std::to_string(gen());
        auto candidate = fs::temp_directory_path() / name;
        std::error_code ec;
        if (fs::create_directory(candidate, ec))
        {
            path = (candidate / "s").string();
            break;
        }
    }
    if (path.empty())
        throw std::runtime_error("make_local_stream_pair: temp path failed");

    auto ex = ctx.get_executor();

    Acceptor acc(ctx);
    acc.open();
    if (auto ec = acc.bind(local_endpoint(path)))
        throw std::runtime_error(
            "local_stream_pair bind failed: " + ec.message());
    if (auto ec = acc.listen())
        throw std::runtime_error(
            "local_stream_pair listen failed: " + ec.message());

    Socket s1(ctx);
    Socket s2(ctx);
    s2.open();

    std::error_code accept_ec, connect_ec;
    bool accept_done = false, connect_done = false;

    capy::run_async(ex)(
        [](Acceptor& a, Socket& s, std::error_code& ec_out,
           bool& done_out) -> capy::task<> {
            auto [ec] = co_await a.accept(s);
            ec_out    = ec;
            done_out  = true;
        }(acc, s1, accept_ec, accept_done));

    capy::run_async(ex)(
        [](Socket& s, local_endpoint ep, std::error_code& ec_out,
           bool& done_out) -> capy::task<> {
            auto [ec] = co_await s.connect(ep);
            ec_out    = ec;
            done_out  = true;
        }(s2, local_endpoint(path), connect_ec, connect_done));

    ctx.run();
    ctx.restart();

    // The bind path on disk is no longer needed once accept/connect
    // have rendezvoused; remove the file and its parent directory so
    // repeated bench invocations don't accumulate cruft under /tmp.
    std::error_code rm_ec;
    fs::remove(fs::path(path), rm_ec);
    fs::remove(fs::path(path).parent_path(), rm_ec);

    if (!accept_done || accept_ec)
    {
        std::fprintf(
            stderr, "local_stream_pair: accept failed (done=%d, ec=%s)\n",
            accept_done, accept_ec.message().c_str());
        acc.close();
        throw std::runtime_error("local_stream_pair accept failed");
    }

    if (!connect_done || connect_ec)
    {
        std::fprintf(
            stderr, "local_stream_pair: connect failed (done=%d, ec=%s)\n",
            connect_done, connect_ec.message().c_str());
        acc.close();
        s1.close();
        throw std::runtime_error("local_stream_pair connect failed");
    }

    acc.close();

    return {std::move(s1), std::move(s2)};
}

} // namespace boost::corosio::test

#endif
