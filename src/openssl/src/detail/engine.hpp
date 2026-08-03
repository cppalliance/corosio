//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef SRC_OPENSSL_DETAIL_ENGINE_HPP
#define SRC_OPENSSL_DETAIL_ENGINE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/tls_context.hpp>
#include <boost/corosio/tls_stream.hpp>

#include "src/tls/detail/engine_types.hpp"

#include <cstddef>
#include <string>
#include <system_error>
#include <utility>

// Opaque OpenSSL session handles, mirroring the vendor's own typedef
// targets (`typedef struct ssl_st SSL` / `typedef struct bio_st
// BIO`). This header stays vendor-free so a TU may hold both
// backends' engines: the real OpenSSL and WolfSSL headers cannot
// coexist (WolfSSL's compatibility layer clashes with genuine
// OpenSSL declarations).
struct ssl_st;
struct bio_st;

namespace boost::corosio {

namespace detail {

class openssl_native_context;

// Backend scope: both backends spell their engine `engine`, and both
// libraries (plus their unit tests) link into one binary, so each
// class needs a distinct qualified name.
namespace openssl {

/** Synchronous, transport-free OpenSSL record engine.

    Owns the SSL session and its byte interface (a memory BIO pair)
    and concentrates every SSL-result-to-error-code decision in one
    mapping site (`perform`). The coroutine driver keeps the
    transport, claims, and buffering; it only shuttles bytes through
    `put_input` / `get_output` as directed by `engine_want` verdicts.

    Data flow through the BIO pair:

        App -> SSL_write -> int_bio -> get_output -> transport write
        App <- SSL_read  <- int_bio <- put_input  <- transport read

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe.
*/
// Exported so the transport-free engine unit tests can link against
// shared library builds (hidden visibility / DLL boundaries).
class BOOST_COROSIO_DECL engine
{
    ssl_st* ssl_     = nullptr;
    bio_st* ext_bio_ = nullptr;

    // Cached at init; the per-context cache returns the same object
    // for the stream's context every time, so one lookup suffices.
    openssl_native_context* nc_ = nullptr;

    // Set when SSL_clear() or SSL_set_session() fails in reset().
    // Neither has a documented partial-failure contract, so the SSL*
    // is left in an unknown (or still-resumable) state; the driver
    // refuses the next handshake instead of resuming on it. Engine-
    // owned because only engine-internal operations can latch it.
    bool clear_failed_ = false;

public:
    /** Whether a failed transport write keeps drained ciphertext.

        Ciphertext the driver already drained from the engine when a
        transport write fails is dropped; a later flush does not
        resend it.
    */

    /// Destroy the engine, releasing the session and BIO pair.
    ~engine();

    engine() = default;
    engine(engine const&)            = delete;
    engine& operator=(engine const&) = delete;

    /** Create the SSL session and BIO pair from a TLS context.

        Must succeed before any other member is used. A context whose
        native build failed is reported unconditionally: the cache
        retains a failed build permanently and the error queue may
        already be drained, so a queue-derived code could read as
        success.

        @param ctx The TLS context supplying the native `SSL_CTX`.

        @return An error if the session could not be created.
    */
    std::error_code init(tls_context const& ctx);

    /** Reset the session for a fresh handshake.

        Preserves the `SSL*` and BIO pair, releases session state,
        drops the negotiated session (a resumed handshake would skip
        certificate/hostname re-verification), and drains stale bytes
        from the output BIO. Failures latch `clear_failed()`.
    */
    void reset();

    /// Check whether a prior `reset()` left the session unusable.
    bool
    clear_failed() const noexcept
    {
        return clear_failed_;
    }

    /// Check whether the native context build rejected its configuration.
    bool context_setup_failed() const noexcept;

    /** Check that the native context can back a handshake.

        A requested configuration could not be applied when the
        native context was built (inverted protocol window, rejected
        cipher/version, or an unparseable CRL); refuse the handshake
        rather than proceed with weakened or unexpected settings.

        @return An error when the context build rejected its
        configuration.
    */
    std::error_code check_context() const noexcept;

    /** Check that the session survived its last reset.

        A failed `reset()` leaves the session in an unknown (or
        still-resumable) state; refuse the next handshake rather than
        resume on the unknown remainder of a failed clear.

        @return An error when a prior `reset()` failed.
    */
    std::error_code check_session() const noexcept;

    /** Prepare the session for a handshake in the given role.

        Applies SNI/hostname verification and installs the context's
        ALPN offer, both for client handshakes only; a server
        handshake clears any name left by a prior client-role
        handshake so client certificates are never hostname-matched.
        Fails closed rather than handshake without a requested check.

        @param ctx Unused; the session was built from it at `init`.
        @param role Handshake role.
        @param hostname Peer name for SNI/verification; empty for
        none.

        @return An error when a requested setting could not be
        applied.
    */
    std::error_code prepare(
        tls_context const& ctx, tls_role role, std::string const& hostname);

    /** Apply SNI and hostname verification for the next handshake.

        An empty hostname clears any previously applied name. IP
        literals are excluded from SNI and matched against the
        certificate's iPAddress entries instead of its DNS names.

        @param hostname Peer name, or empty to clear.

        @return `true` on success.
    */
    bool apply_hostname(std::string const& hostname);

    /** Install the context's ALPN offer on the session.

        No-op success when the context configured no protocols. Only
        meaningful for client handshakes.

        @return `true` when the offer (if any) was installed.
    */
    bool apply_alpn_offer();

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

        @return The number of bytes accepted; may be zero when the
        staging BIO is full.
    */
    std::size_t put_input(unsigned char const* data, std::size_t len);

    /** Return the writable staging region for a zero-copy transport read.

        The transport reads ciphertext directly into the returned span,
        then reports how much landed via `input_committed`, avoiding the
        copy a `put_input` deposit would incur. The region is the
        contiguous run of the BIO pair's buffer, so its size may be less
        than the total free space near the buffer's wrap.

        @return Pointer and size of the contiguous writable region; the
        size is zero when the staging BIO is full.
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

        @return The number of bytes drained; zero when nothing could
        be read.
    */
    std::size_t get_output(unsigned char* data, std::size_t len);

    /// Check whether the peer's close_notify has been received.
    bool received_shutdown() const;

    /// Return the underlying session handle (tests only).
    ssl_st*
    native_handle() const noexcept
    {
        return ssl_;
    }
};

} // namespace openssl

} // namespace detail

} // namespace boost::corosio

#endif
