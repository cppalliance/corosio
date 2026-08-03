//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef SRC_WOLFSSL_DETAIL_ENGINE_HPP
#define SRC_WOLFSSL_DETAIL_ENGINE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/tls_context.hpp>
#include <boost/corosio/tls_stream.hpp>

#include "src/tls/detail/engine_types.hpp"

#include <cstddef>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// Opaque WolfSSL session handle, mirroring the vendor's own typedef
// target (`typedef struct WOLFSSL WOLFSSL`). This header stays
// vendor-free so a TU may hold both backends' engines: the real
// OpenSSL and WolfSSL headers cannot coexist (WolfSSL's
// compatibility layer clashes with genuine OpenSSL declarations).
struct WOLFSSL;

namespace boost::corosio {

namespace detail {

// Backend scope: both backends spell their engine `engine`, and both
// libraries (plus their unit tests) link into one binary, so each
// class needs a distinct qualified name.
namespace wolfssl {

/** Synchronous, transport-free WolfSSL record engine.

    Owns the WOLFSSL session and its byte interface (a pair of staging
    buffers serviced by WolfSSL I/O callbacks) and concentrates every
    WolfSSL-result-to-error-code decision in one mapping site
    (`perform`). The coroutine driver keeps the transport, claims, and
    buffering; it only shuttles bytes through `put_input` /
    `get_output` as directed by `engine_want` verdicts.

    Data flow through the staging buffers:

        App -> wolfSSL_write -> send cb -> out_ -> get_output -> transport
        App <- wolfSSL_read  <- recv cb <- in_  <- put_input  <- transport

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe.
*/
// Exported so the transport-free engine unit tests can link against
// shared library builds (hidden visibility / DLL boundaries).
class BOOST_COROSIO_DECL engine
{
    WOLFSSL* ssl_ = nullptr;

    // Input staging consumed by the recv callback and output staging
    // filled by the send callback. Wire bytes must be consumed and
    // produced in order regardless of which operation's engine call
    // touches them, so per-operation buffers cannot work.
    BOOST_COROSIO_MSVC_WARNING_PUSH
    BOOST_COROSIO_MSVC_WARNING_DISABLE(4251) // std::vector, dll-interface
    std::vector<char> in_;
    std::size_t in_pos_ = 0;
    std::size_t in_len_ = 0;
    std::vector<char> out_;
    std::size_t out_len_ = 0;
    BOOST_COROSIO_MSVC_WARNING_POP

    // Some wolfSSL builds clear the shutdown bitmask on a read that
    // follows a completed close, so the received-close_notify state is
    // latched here the moment it is first observed rather than re-queried
    // from the session on every access.
    bool received_close_notify_ = false;

    static int recv_callback(WOLFSSL*, char* buf, int sz, void* ctx);
    static int send_callback(WOLFSSL*, char* buf, int sz, void* ctx);

public:
    /** Whether a failed transport write keeps drained ciphertext.

        Ciphertext the driver already drained from the engine when a
        transport write fails stays queued, and the next flush resends
        exactly that unsent tail, so a canceled flush can resume
        without record loss.
    */

    /// Destroy the engine, releasing the session.
    ~engine();

    /// Construct an engine with empty staging buffers.
    engine();

    engine(engine const&)            = delete;
    engine& operator=(engine const&) = delete;

    /** Create the WolfSSL session for a handshake role.

        WolfSSL requires role-specific method functions, so session
        creation is deferred until the role is known; a no-op success
        when a session already exists. Applies every per-session
        setting: I/O callbacks, the verify-callback capability gate,
        ALPN, the CRL capability gate, and client SNI/hostname
        verification. Any setting that cannot be honored fails closed
        with no session left behind.

        @param ctx The TLS context supplying the cached native contexts.
        @param role Handshake role selecting the native context.
        @param hostname Peer name for SNI/verification; empty for none.

        @return An error if the session could not be created or
        configured.
    */
    std::error_code
    init(tls_context const& ctx, tls_role role, std::string const& hostname);

    /** Release the session and clear the staging buffers.

        The next `init` builds a fresh session, so a rechanged
        hostname or role is honored. Staging allocations are kept.
    */
    void reset();

    /// Nothing can invalidate the cached contexts between handshakes.
    std::error_code
    check_context() const noexcept
    {
        return {};
    }

    /// Session teardown in `reset()` cannot fail.
    std::error_code
    check_session() const noexcept
    {
        return {};
    }

    /** Prepare the session for a handshake in the given role.

        Builds the session from the role's cached native context and
        applies every per-session setting; see `init`.

        @param ctx The TLS context supplying the cached native
        contexts.
        @param role Handshake role selecting the native context.
        @param hostname Peer name for SNI/verification; empty for
        none.

        @return An error if the session could not be created or
        configured.
    */
    std::error_code
    prepare(tls_context const& ctx, tls_role role, std::string const& hostname)
    {
        return init(ctx, role, hostname);
    }

    /** Record the ALPN protocol selected during the handshake.

        Assigns `out` only when a protocol was negotiated, leaving it
        untouched otherwise.

        @param out Receives the selected protocol.
    */
    void capture_alpn(std::string& out) const;

    /** Run one synchronous engine step and map its outcome.

        All error mapping lives here: a `done` verdict with a truthy
        `ec` is terminal and already mapped. Output-first: when the
        step leaves pending output, the verdict is
        `output_then_retry` / `output_then_done` so ciphertext
        reaches the peer before the driver parks on input. A received
        close_notify (`read` / `write`) reports `eof` with a plain
        `done`: it queues no output, so no flush precedes it.

        @param op Which operation to advance.
        @param data Application buffer (`read` / `write` only).
        @param len Application buffer size in bytes.

        @return The mapped verdict for this step.
    */
    engine_result perform(engine_op op, void* data, std::size_t len);

    /** Stage transport bytes into the engine.

        @param data Bytes received from the transport.
        @param len Number of bytes offered.

        @return The number of bytes accepted; may be less than `len`
        (zero when the staging buffer is full).
    */
    std::size_t put_input(unsigned char const* data, std::size_t len);

    /** Return the writable staging region for a zero-copy transport read.

        The transport reads ciphertext directly into the returned span,
        then reports how much landed via `input_committed`, avoiding the
        copy a `put_input` deposit would incur.

        @return Pointer and capacity of the contiguous writable region;
        the capacity is zero when the staging buffer is full.
    */
    std::pair<unsigned char*, std::size_t> input_area();

    /** Commit bytes the transport read into the `input_area` span.

        @param n Number of bytes written into the region.
    */
    void input_committed(std::size_t n);

    /// Return the number of ciphertext bytes awaiting transport write.
    std::size_t pending_output() const;

    /** Drain staged ciphertext for transport write.

        @param data Destination buffer.
        @param len Destination capacity in bytes.

        @return The number of bytes drained.
    */
    std::size_t get_output(unsigned char* data, std::size_t len);

    /// Check whether the peer's close_notify has been received.
    bool received_shutdown() const;

    /// Return the underlying session handle (tests only).
    WOLFSSL*
    native_handle() const noexcept
    {
        return ssl_;
    }
};

} // namespace wolfssl

} // namespace detail

} // namespace boost::corosio

#endif
