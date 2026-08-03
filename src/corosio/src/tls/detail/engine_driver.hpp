//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef SRC_TLS_DETAIL_ENGINE_DRIVER_HPP
#define SRC_TLS_DETAIL_ENGINE_DRIVER_HPP

#include <boost/corosio/tls_context.hpp>
#include <boost/corosio/tls_stream.hpp>

#include "src/tls/detail/engine_types.hpp"

#include <boost/capy/buffers.hpp>
#include <boost/capy/detail/buffer_array.hpp>
#include <boost/capy/ex/async_mutex.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// This header is instantiated in both backends' TUs, whose vendor
// headers cannot coexist (WolfSSL's OpenSSL-compat layer clashes
// with genuine OpenSSL declarations on some builds); it must stay
// vendor-free.
#ifdef OPENSSL_VERSION_NUMBER
#error engine_driver.hpp must stay vendor-header-free
#endif
#ifdef WOLFSSL_VERSION
#error engine_driver.hpp must stay vendor-header-free
#endif

/*
    TLS Driver Architecture

    TLS layer wrapping an underlying stream (via any_stream). One
    read_some and one write_some may be in flight concurrently.

    Engine / Driver Split. The engine (each backend's detail/engine.hpp)
    owns the TLS session, its byte interface, and every library-result-
    to-error-code decision; it is synchronous and transport-free. This
    driver owns the transport, the per-direction claims, and all
    buffering, and loops on engine verdicts:

      1. Call eng_.perform(op, data, len)
      2. output_then_retry: flush pending ciphertext, then retry
      3. input: feed transport bytes into the engine, then retry
      4. done / output_then_done: flush if pending, report the
         (already mapped) ec

    Renegotiation causes cross-direction I/O: a read may need to
    write handshake data, a write may need to read. Each operation
    services whatever direction the engine requests.

    Full-Duplex Concurrency. rd_cm_ and wr_cm_ are separate transport
    claims: a read parked on the network cannot stall a concurrent
    write's flush, and vice versa. A cross-direction request (e.g. a
    read needing to flush queued handshake output) takes the other
    direction's claim itself.

    read_gen_ counts successful deposits into the engine. Each engine
    call snapshots it before running; if fill_input later finds the
    generation has moved (a concurrent operation's flush suspension let
    a deposit land in the meantime) it retries the engine instead of
    issuing a redundant transport read.

    The transport reads ciphertext straight into the engine's input
    staging via input_area(), so no driver-side buffer or deposit copy
    sits on the read path. The write path stays copy-based on purpose:
    ciphertext drained from the engine into out_buf_[0, out_len_) is
    decoupled from the engine's shared output staging, so a concurrent
    reader can still emit its own records (an alert, a key update) while
    a write is in flight. out_len_ holds only the abandoned tail a failed
    or canceled write kept for retry; the length being written is a local,
    never published, so a concurrent flush probe cannot mistake in-flight
    bytes for a tail and park on the write claim behind a healthy write.

    A read or write that fully satisfies the caller's buffer still owes
    a trailing flush (queued handshake or session-ticket output must
    reach the peer before the call can report success). If that flush
    fails, the transfer already succeeded and the stream contract
    forbids reporting both a full count and an error in the same
    result, so the error is stashed in pending_flush_ec_ and surfaced
    by the next read, write, or shutdown call instead.
*/

namespace boost::corosio {

namespace detail {

/** The engine surface `engine_driver` drives.

    A synchronous, transport-free TLS record engine: `perform`
    advances one operation and returns a fully mapped verdict, while
    `put_input` / `get_output` shuttle wire bytes. The handshake
    hooks (`check_context`, `check_session`, `prepare`) let each
    backend gate and configure a handshake at the protocol-mandated
    points without the driver knowing the backend's session model.
*/
template<class Engine>
concept tls_engine =
    requires(
        Engine& e,
        Engine const& ce,
        engine_op op,
        void* buf,
        unsigned char const* in,
        unsigned char* out,
        std::size_t n,
        tls_context const& ctx,
        tls_role role,
        std::string const& hostname,
        std::string& alpn)
    {
        { e.perform(op, buf, n) } -> std::same_as<engine_result>;
        { e.put_input(in, n) } -> std::same_as<std::size_t>;
        { e.input_area() }
            -> std::same_as<std::pair<unsigned char*, std::size_t>>;
        { e.input_committed(n) };
        { ce.pending_output() } -> std::same_as<std::size_t>;
        { e.get_output(out, n) } -> std::same_as<std::size_t>;
        { ce.received_shutdown() } -> std::same_as<bool>;
        { ce.capture_alpn(alpn) };
        { e.reset() };
        { ce.check_context() } -> std::same_as<std::error_code>;
        { ce.check_session() } -> std::same_as<std::error_code>;
        { e.prepare(ctx, role, hostname) } -> std::same_as<std::error_code>;
    };

/** Coroutine driver shared by every TLS backend.

    Hosts the transport, the per-direction claims, the buffering, and
    the four operation loops; the engine type supplies all TLS
    mechanics. Each backend's stream impl instantiates this template
    with its own engine so the driver logic exists exactly once.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe.
*/
template<tls_engine Engine>
class engine_driver
{
    // Large enough to hold the largest possible TLS record.
    static constexpr std::size_t buffer_size_ = std::size_t{17} * 1024;

    capy::any_stream* s_;
    tls_context ctx_;
    Engine eng_;

    // A handshake was attempted (successfully or not); the stream
    // must be reset before the next handshake.
    bool used_ = false;

    // Per-stream SNI/verification hostname, set via set_hostname().
    std::string hostname_;

    // ALPN protocol negotiated during the handshake (empty if none).
    std::string alpn_selected_;

    std::vector<char> out_buf_;

    // One transport claim per direction so a parked reader cannot block
    // a writer's flush. read_gen_ counts deposits into the engine: each
    // operation captures it at its engine call and passes it to
    // fill_input, which retries the engine instead of issuing a stale
    // transport read when a deposit landed anywhere after the engine's
    // verdict (including during an intervening flush suspension).
    capy::async_mutex rd_cm_;
    capy::async_mutex wr_cm_;
    std::uint64_t read_gen_ = 0;

    // Engine ciphertext drained but not yet written: the abandoned
    // tail a failed or canceled transport write kept for retry.
    // Zero while a write is in flight (that length is a local).
    std::size_t out_len_ = 0;

    // A trailing flush that fails after the engine already accepted a
    // full payload cannot be reported alongside n == size (the stream
    // contract forbids error + full transfer, and capy::write's
    // composed loop discards ec exactly in that case); the error is
    // held here and surfaced by the next operation instead.
    std::error_code pending_flush_ec_;

    capy::task<std::error_code> flush_output()
    {
        if (eng_.pending_output() == 0 && out_len_ == 0)
            co_return std::error_code{};

        auto [lec] = co_await wr_cm_.lock();
        if (lec)
            co_return lec;
        capy::async_mutex::lock_guard wr_guard(&wr_cm_);

        // Drain under the claim so records reach the wire in engine
        // order even when both directions produce output.
        while (eng_.pending_output() > 0 || out_len_ > 0)
        {
            // Take the abandoned tail (from an earlier failed write in
            // this loop or a prior flush) into a local and append fresh
            // engine ciphertext behind it, preserving record order.
            // out_len_ drops to zero for the duration of the write: the
            // in-flight length lives only in `n`, so a concurrent flush
            // probe never sees these bytes as a tail and never parks on
            // the write claim behind a healthy write.
            std::size_t n = out_len_;
            out_len_      = 0;
            if (n < out_buf_.size())
                n += eng_.get_output(
                    reinterpret_cast<unsigned char*>(out_buf_.data()) + n,
                    out_buf_.size() - n);
            // The loop guard just confirmed pending bytes exist, so a
            // drain failure here is unreachable in practice; fail loudly
            // rather than silently drop already-accepted ciphertext.
            if (n == 0)
                co_return make_error_code(std::errc::no_buffer_space);
            auto [ec, wn] = co_await capy::write(
                *s_, capy::const_buffer(out_buf_.data(), n));
            if (ec)
            {
                // wn bytes already reached the peer; keep only the unsent
                // remainder so a post-cancellation flush retry resends
                // neither the delivered prefix nor loses the rest.
                std::memmove(
                    out_buf_.data(), out_buf_.data() + wn, n - wn);
                out_len_ = n - wn;
                co_return ec;
            }
        }
        co_return std::error_code{};
    }

    // Flushes on a fatal/alert exit path: the engine may have queued a
    // close_notify or alert that should still reach the peer, but a
    // caller already reporting the triggering error cannot also report
    // this flush's own failure. flush_output() itself no-ops when
    // nothing is pending.
    capy::task<void> best_effort_flush()
    {
        (void)co_await flush_output();
    }

    // gen is the caller's read_gen_ snapshot from its engine call; a
    // later snapshot would miss deposits landing while the caller's
    // flush_output was suspended.
    capy::task<std::error_code> fill_input(std::uint64_t gen)
    {
        // Input already arrived since the engine's verdict; retry the
        // engine rather than queue behind a re-parked reader.
        if (read_gen_ != gen)
            co_return std::error_code{};

        auto [lec] = co_await rd_cm_.lock();
        if (lec)
            co_return lec;
        capy::async_mutex::lock_guard rd_guard(&rd_cm_);

        // Input arrived while we queued for the claim; the caller's
        // engine retry consumes it.
        if (read_gen_ != gen)
            co_return std::error_code{};

        // Read the transport straight into the engine's input staging:
        // input_area() hands back the contiguous writable run, so no
        // staging buffer or deposit copy sits between the socket and the
        // engine.
        auto [dst, cap] = eng_.input_area();
        // The staging is full while the caller still wants input: its own
        // engine retry must decrypt the staged record to free space, so
        // report success to re-run it rather than issue a zero-length read.
        if (cap == 0)
            co_return std::error_code{};

        auto [ec, n] =
            co_await s_->read_some(capy::mutable_buffer(dst, cap));

        // ReadStream permits n>0 alongside ec (IOCP forwards
        // bytes_transferred on failed completions; a canceled read can
        // likewise deliver bytes); commit whatever arrived before
        // surfacing ec, or the record stream loses wire bytes the
        // transport already handed over.
        if (n > 0)
        {
            eng_.input_committed(n);
            ++read_gen_;
            co_return ec;
        }

        if (ec)
            co_return ec;

        // The transport delivered nothing without an error, so it cannot
        // make progress: fail loudly rather than spin the engine's input
        // retry against a staging that will never fill.
        co_return make_error_code(std::errc::no_buffer_space);
    }

    // A prior read/write already reported its full transfer as success;
    // the trailing flush error it deferred surfaces on the next call
    // instead. Each entry point takes it exactly once.
    std::error_code take_pending_flush_ec() noexcept
    {
        std::error_code ec = pending_flush_ec_;
        pending_flush_ec_  = {};
        return ec;
    }

public:
    /** Construct a driver over a transport stream.

        @param s The transport; the caller keeps it alive and repoints
        it on move via `rebind_stream`.
        @param ctx The TLS context handed to the engine's handshake
        preparation.
    */
    engine_driver(capy::any_stream& s, tls_context ctx)
        : s_(&s)
        , ctx_(std::move(ctx))
    {
        out_buf_.resize(buffer_size_);
    }

    /// Return the engine for backend-specific setup.
    Engine&
    engine() noexcept
    {
        return eng_;
    }

    /// Return the TLS context this driver was constructed with.
    tls_context const&
    context() const noexcept
    {
        return ctx_;
    }

    /// Point the driver at the transport's post-move location.
    void
    rebind_stream(capy::any_stream& s) noexcept
    {
        s_ = &s;
    }

    /// Set the hostname applied to the next client handshake.
    void
    set_hostname(std::string_view hostname)
    {
        hostname_ = hostname;
    }

    /// Return the ALPN protocol negotiated by the last handshake.
    std::string_view
    alpn_protocol() const noexcept
    {
        return alpn_selected_;
    }

    /// Reset the engine and every per-connection driver state.
    void reset()
    {
        eng_.reset();

        out_len_ = 0;

        alpn_selected_.clear();
        pending_flush_ec_ = {};
        used_             = false;
    }

    capy::io_task<std::size_t>
    do_read_some(
        capy::detail::mutable_buffer_array<capy::detail::max_iovec_> buffers)
    {
        if (auto ec = take_pending_flush_ec())
            co_return {ec, 0};

        std::error_code ec;
        std::size_t total_read      = 0;
        std::size_t const bufs_size = capy::buffer_size(buffers);

        for (auto& buf : buffers)
        {
            char* const dest    = static_cast<char*>(buf.data());
            int const remaining = static_cast<int>(buf.size());
            if (remaining == 0)
                continue;

            // Exits by co_return: success after the first transferred
            // chunk, or any terminal error.
            while (true)
            {
                auto const gen = read_gen_;
                auto r         = eng_.perform(
                    engine_op::read, dest,
                    static_cast<std::size_t>(remaining));

                if (r.ec)
                {
                    // Terminal, already mapped by the engine. eof (a
                    // received close_notify) arrives as a plain done:
                    // it queues no output, so no flush is needed.
                    if (r.want == engine_want::output_then_done)
                        co_await best_effort_flush();
                    co_return {r.ec, total_read};
                }

                if (r.want == engine_want::done ||
                    r.want == engine_want::output_then_done)
                {
                    total_read += r.bytes;

                    // r.bytes > 0 already satisfies ReadStream's "at
                    // least one byte transferred" success condition;
                    // report now rather than loop for more (another
                    // engine call could park on input). out_len_ > 0
                    // covers a retained tail the engine cannot see.
                    if (r.want == engine_want::output_then_done ||
                        out_len_ > 0)
                        ec = co_await flush_output();
                    if (ec && total_read == bufs_size)
                    {
                        // First failure wins: concurrent directions share
                        // one transport failure domain; the earliest
                        // error is the meaningful one.
                        if (!pending_flush_ec_)
                            pending_flush_ec_ = ec;
                        ec = {};
                    }
                    co_return {ec, total_read};
                }

                if (r.want == engine_want::output_then_retry)
                {
                    ec = co_await flush_output();
                    if (ec)
                        co_return {ec, total_read};
                    continue;
                }

                // want == input. Flush first: a retained tail (an
                // earlier failed write) may hold bytes the peer needs
                // before it will send more; no-op when nothing pends.
                ec = co_await flush_output();
                if (ec)
                    co_return {ec, total_read};

                ec = co_await fill_input(gen);
                if (ec)
                {
                    ec = map_fill_error(
                        engine_op::read, ec, eng_.received_shutdown());
                    co_return {ec, total_read};
                }
            }
        }

        co_return {std::error_code{}, total_read};
    }

    capy::io_task<std::size_t>
    do_write_some(
        capy::detail::const_buffer_array<capy::detail::max_iovec_> buffers)
    {
        if (auto ec = take_pending_flush_ec())
            co_return {ec, 0};

        std::error_code ec;
        std::size_t total_written   = 0;
        std::size_t const bufs_size = capy::buffer_size(buffers);

        for (auto const& buf : buffers)
        {
            // The engine only reads through this pointer for a write
            // op; the cast satisfies perform's single transfer
            // signature.
            void* const src     = const_cast<void*>(buf.data());
            int const remaining = static_cast<int>(buf.size());
            if (remaining == 0)
                continue;

            // Exits by co_return: success after the first transferred
            // chunk, or any terminal error.
            while (true)
            {
                auto const gen = read_gen_;
                auto r         = eng_.perform(
                    engine_op::write, src,
                    static_cast<std::size_t>(remaining));

                if (r.ec)
                {
                    // Terminal, already mapped. eof means the peer's
                    // close_notify WAS received (an announced close);
                    // it arrives as a plain done and needs no flush.
                    if (r.want == engine_want::output_then_done)
                        co_await best_effort_flush();
                    co_return {r.ec, total_written};
                }

                if (r.want == engine_want::done ||
                    r.want == engine_want::output_then_done)
                {
                    total_written += r.bytes;

                    // r.bytes > 0 already satisfies WriteStream's "at
                    // least one byte transferred" success condition;
                    // report now rather than loop for more. out_len_ > 0
                    // covers a retained tail the engine cannot see.
                    if (r.want == engine_want::output_then_done ||
                        out_len_ > 0)
                        ec = co_await flush_output();
                    if (ec && total_written == bufs_size)
                    {
                        // First failure wins: concurrent directions share
                        // one transport failure domain; the earliest
                        // error is the meaningful one.
                        if (!pending_flush_ec_)
                            pending_flush_ec_ = ec;
                        ec = {};
                    }
                    co_return {ec, total_written};
                }

                if (r.want == engine_want::output_then_retry)
                {
                    ec = co_await flush_output();
                    if (ec)
                        co_return {ec, total_written};
                    continue;
                }

                // want == input: a write can need a read mid-rekey; the
                // transport eof this surfaces means the same thing it
                // means on the read path, so map it the same way. Flush
                // first for the same retained-tail reason as the read
                // path.
                ec = co_await flush_output();
                if (ec)
                    co_return {ec, total_written};

                ec = co_await fill_input(gen);
                if (ec)
                {
                    ec = map_fill_error(
                        engine_op::write, ec, eng_.received_shutdown());
                    co_return {ec, total_written};
                }
            }
        }

        co_return {std::error_code{}, total_written};
    }

    capy::io_task<> do_handshake(tls_role role)
    {
        // Refuse the handshake while the engine's configuration is
        // unusable, before consuming any per-connection state.
        if (auto cec = eng_.check_context())
            co_return {cec};

        if (used_)
            reset();

        // reset() may not have restored a usable session; refuse to
        // hand out a handshake on it.
        if (auto sec = eng_.check_session())
            co_return {sec};

        // A failed attempt leaves the session in a dead state; any
        // attempt, not just a completed handshake, consumes the stream
        // so the next handshake() starts fresh.
        used_ = true;

        if (auto pec = eng_.prepare(ctx_, role, hostname_))
            co_return {pec};

        auto const op = role == tls_role::client
            ? engine_op::handshake_client
            : engine_op::handshake_server;

        std::error_code ec;

        while (true)
        {
            auto const gen = read_gen_;
            auto r         = eng_.perform(op, nullptr, 0);

            if (r.ec)
            {
                if (r.want == engine_want::output_then_done)
                    co_await best_effort_flush();
                co_return {r.ec};
            }

            if (r.want == engine_want::done ||
                r.want == engine_want::output_then_done)
            {
                eng_.capture_alpn(alpn_selected_);
                ec = co_await flush_output();
                co_return {ec};
            }

            if (r.want == engine_want::output_then_retry)
            {
                // Must flush (e.g. ClientHello) before reading the
                // peer's reply.
                ec = co_await flush_output();
                if (ec)
                    co_return {ec};
                continue;
            }

            // want == input. The flush is a no-op unless a retained
            // tail pends; the fill error passes through map_fill_error
            // unchanged (no close is clean before the session is
            // established).
            ec = co_await flush_output();
            if (ec)
                co_return {ec};

            ec = co_await fill_input(gen);
            if (ec)
            {
                ec = map_fill_error(op, ec, eng_.received_shutdown());
                co_return {ec};
            }
        }
    }

    capy::io_task<> do_shutdown()
    {
        if (auto ec = take_pending_flush_ec())
            co_return {ec};

        std::error_code ec;

        while (true)
        {
            auto const gen = read_gen_;
            auto r = eng_.perform(engine_op::shutdown, nullptr, 0);

            if (r.ec)
            {
                if (r.want == engine_want::output_then_done)
                    co_await best_effort_flush();
                co_return {r.ec};
            }

            if (r.want == engine_want::done ||
                r.want == engine_want::output_then_done)
            {
                // Covers both the bidirectional-complete result and the
                // engine's shutdown-after-close success mapping.
                ec = co_await flush_output();
                co_return {ec};
            }

            if (r.want == engine_want::output_then_retry)
            {
                // Sends our close_notify before parking for the peer's.
                ec = co_await flush_output();
                if (ec)
                    co_return {ec};
                continue;
            }

            // want == input: awaiting the peer's close_notify. It may
            // already have been deposited (and consumed by a concurrent
            // reader) during a flush; fill_input then returns without
            // reading and the loop retries the engine instead of parking.
            ec = co_await flush_output();
            if (ec)
                co_return {ec};

            ec = co_await fill_input(gen);
            if (ec)
            {
                ec = map_fill_error(
                    engine_op::shutdown, ec, eng_.received_shutdown());
                co_return {ec};
            }
        }
    }
};

} // namespace detail

} // namespace boost::corosio

#endif
