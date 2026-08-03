//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef SRC_TLS_DETAIL_ENGINE_TYPES_HPP
#define SRC_TLS_DETAIL_ENGINE_TYPES_HPP

#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/ipv6_address.hpp>

#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>

#include <cstddef>
#include <string>
#include <system_error>

namespace boost::corosio {

namespace detail {

/** What the TLS engine needs from its driver after a `perform` call.

    The driver owns the transport; the engine only stages bytes. Each
    verdict tells the driver which transport action unblocks the
    engine before the operation can make progress.
*/
enum class engine_want
{
    /// The operation finished; `ec` and `bytes` carry the outcome.
    done,

    /// The engine needs more input bytes before it can retry.
    input,

    /// Pending output must reach the peer, then retry the operation.
    output_then_retry,

    /// Pending output must reach the peer; the operation finished.
    output_then_done
};

/// Outcome of one synchronous engine step.
struct engine_result
{
    /// Required driver action.
    engine_want want;

    /// Mapped, final error for `done` verdicts; empty otherwise.
    std::error_code ec;

    /// Application bytes transferred by this step.
    std::size_t bytes;
};

/// Operation selector for `engine::perform`.
enum class engine_op
{
    handshake_client,
    handshake_server,
    read,
    write,
    shutdown
};

/** Check whether a peer name is an IP literal rather than a DNS name.

    RFC 6066 excludes IP literals from SNI, and a literal must be matched
    against a certificate's iPAddress entries rather than its DNS names,
    so both backends branch on this when applying hostname verification.

    @param s The peer name.

    @return `true` when `s` parses as an IPv4 or IPv6 address.
*/
inline bool
is_ip_literal(std::string const& s) noexcept
{
    ipv4_address v4;
    ipv6_address v6;
    return !parse_ipv4_address(s, v4) || !parse_ipv6_address(s, v6);
}

/** Map a transport error observed while filling engine input.

    What a transport failure means depends on which operation was
    starved of input and on close_notify visibility, but not on the
    backend; the whole policy lives once here.

    For `read` / `write`, a transport end-of-stream after the peer's
    close_notify is a clean `eof`; without one it is a truncation
    attack per the TLS contract. Other errors pass through unchanged.

    For `shutdown`, a member of the eof/reset/aborted/broken_pipe
    family may reflect the peer's close_notify that a concurrent
    reader already consumed during the shutdown's flush rather than a
    genuine truncation; the received-shutdown bit distinguishes a
    completed close (success) from an unannounced one
    (`stream_truncated`). `canceled` and errors outside that family
    pass through unchanged so the received-shutdown race window can
    never absorb an unrelated fault.

    Handshake operations pass every error through: no close is clean
    before the session is established.

    @param op The operation whose input fill failed.
    @param ec The transport error.
    @param received_shutdown Whether the peer's close_notify was seen.

    @return The mapped error.
*/
inline std::error_code
map_fill_error(
    engine_op op, std::error_code ec, bool received_shutdown) noexcept
{
    if (op == engine_op::shutdown)
    {
        if (!ec || ec == capy::cond::canceled)
            return ec;

        if (ec != capy::cond::eof && ec != std::errc::connection_reset &&
            ec != std::errc::connection_aborted &&
            ec != std::errc::broken_pipe)
            return ec;

        if (received_shutdown)
            return {};

        // A peer that closed without a proper close_notify is a
        // truncated stream.
        return make_error_code(capy::error::stream_truncated);
    }

    if ((op == engine_op::read || op == engine_op::write) &&
        ec == capy::cond::eof)
        return make_error_code(
            received_shutdown ? capy::error::eof
                              : capy::error::stream_truncated);

    return ec;
}

} // namespace detail

} // namespace boost::corosio

#endif
