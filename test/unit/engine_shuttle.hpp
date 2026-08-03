//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_UNIT_ENGINE_SHUTTLE_HPP
#define BOOST_COROSIO_TEST_UNIT_ENGINE_SHUTTLE_HPP

#include "src/corosio/src/tls/detail/engine_types.hpp"

#include "test_suite.hpp"

#include <cstddef>
#include <string>

namespace boost::corosio::test {

// Test-side stand-in for the coroutine drivers' transport duties:
// these helpers ferry staged ciphertext between two engines so the
// engines' want protocol can be exercised without an io_context.
// Templates rather than a concrete engine type: the two backends
// spell the same interface, and cross-backend shuttling must work.

/// Check whether a perform verdict finished the operation.
inline bool
engine_step_done(detail::engine_want w)
{
    return w == detail::engine_want::done ||
        w == detail::engine_want::output_then_done;
}

/** Move every staged output byte of `from` into `to`'s input.

    Returns the number of bytes moved. Tests that exercise a full
    destination feed it via `put_input` directly, never through here.
*/
template<class From, class To>
std::size_t
shuttle_bytes(From& from, To& to)
{
    unsigned char buf[4096];
    std::size_t moved = 0;
    while (from.pending_output() > 0)
    {
        std::size_t const n = from.get_output(buf, sizeof(buf));
        if (n == 0)
            break;
        std::size_t off = 0;
        while (off < n)
        {
            std::size_t const put = to.put_input(buf + off, n - off);
            // A full destination cannot drain inside this loop, so
            // bytes already taken from `from` would be lost silently;
            // fail loudly instead of misreporting it as a protocol
            // failure downstream.
            if (!BOOST_TEST(put > 0))
                return moved + off;
            off += put;
        }
        moved += n;
    }
    return moved;
}

/** Drive both handshakes to completion, shuttling between steps.

    Returns `true` when both engines report a successful handshake.
    Any terminal error, or failure to converge, returns `false`.
*/
template<class Client, class Server>
bool
run_engine_handshake(Client& client, Server& server)
{
    for (int i = 0; i < 64; ++i)
    {
        auto const rc =
            client.perform(detail::engine_op::handshake_client, nullptr, 0);
        if (rc.ec)
            return false;
        shuttle_bytes(client, server);
        auto const rs =
            server.perform(detail::engine_op::handshake_server, nullptr, 0);
        if (rs.ec)
            return false;
        shuttle_bytes(server, client);
        if (engine_step_done(rc.want) && engine_step_done(rs.want))
        {
            // Post-handshake flights (session tickets) may still be
            // staged; deliver them so later steps see a quiet wire.
            shuttle_bytes(server, client);
            shuttle_bytes(client, server);
            return true;
        }
    }
    return false;
}

/** Write all of `msg` through `from`, shuttling ciphertext to `to`.

    Returns `true` when every byte was reported written.
*/
template<class From, class To>
bool
engine_send(From& from, To& to, std::string msg)
{
    std::size_t total = 0;
    for (int i = 0; i < 64 && total < msg.size(); ++i)
    {
        auto const r = from.perform(
            detail::engine_op::write, msg.data() + total, msg.size() - total);
        if (r.ec)
            return false;
        shuttle_bytes(from, to);
        if (engine_step_done(r.want))
            total += r.bytes;
        else if (r.want == detail::engine_want::input)
            // Nothing here can feed it; the flow under test never
            // needs a mid-write read.
            return false;
    }
    return total == msg.size();
}

/** Read exactly `n` bytes already staged (or arriving) in `from`.

    Ciphertext produced while reading (for example a key-update
    response) is shuttled to `peer`. Returns `true` when `out` holds
    `n` bytes.
*/
template<class Reader, class Peer>
bool
engine_recv(Reader& from, Peer& peer, std::string& out, std::size_t n)
{
    out.clear();
    char buf[8192];
    for (int i = 0; i < 64 && out.size() < n; ++i)
    {
        auto const r = from.perform(
            detail::engine_op::read, buf,
            n - out.size() < sizeof(buf) ? n - out.size() : sizeof(buf));
        shuttle_bytes(from, peer);
        if (r.ec)
            return false;
        if (engine_step_done(r.want))
            out.append(buf, r.bytes);
        else if (r.want == detail::engine_want::input)
            // Out of staged bytes with nothing left to deliver.
            return false;
    }
    return out.size() == n;
}

/** Drive both engines through a clean bidirectional shutdown.

    Alternates `shutdown` on each engine, shuttling each queued
    close_notify to the peer, until both report a done verdict.
    Returns `false` on any terminal error or failure to converge.
*/
template<class A, class B>
bool
run_engine_shutdown(A& a, B& b)
{
    for (int i = 0; i < 64; ++i)
    {
        auto const ra = a.perform(detail::engine_op::shutdown, nullptr, 0);
        if (ra.ec)
            return false;
        shuttle_bytes(a, b);
        auto const rb = b.perform(detail::engine_op::shutdown, nullptr, 0);
        if (rb.ec)
            return false;
        shuttle_bytes(b, a);
        if (engine_step_done(ra.want) && engine_step_done(rb.want))
            return true;
    }
    return false;
}

} // namespace boost::corosio::test

#endif
