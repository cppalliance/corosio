//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_DGRAM_OPS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_DGRAM_OPS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IO_URING

#include <liburing.h>

#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_op.hpp>
#include <boost/corosio/native/detail/coro_op_complete.hpp>
#include <boost/corosio/native/detail/speculative_state.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_socket_ops.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/capy/error.hpp>

#include <cstddef>
#include <cstdint>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

namespace boost::corosio::detail {

/** Datagram send op — connected and unconnected.

    Always uses `IORING_OP_SENDMSG`. In connected mode, `dest_len == 0`
    and `msg.msg_name == nullptr`. In unconnected mode, `dest_storage`
    holds the destination and `msg.msg_name` points at it.

    `iovec[io_uring_max_iov]` for scatter/gather: a single datagram
    can be assembled from N user buffers via `msg.msg_iov`.
*/
struct uring_dgram_send_op : io_uring_op
{
    iovec            iovecs[io_uring_max_iov];
    int              iovec_count = 0;
    msghdr           msg{};
    sockaddr_storage dest_storage{};
    socklen_t        dest_len  = 0;
    int              fd        = -1;
    int              msg_flags = 0;
    detail::speculative_state* spec_state = nullptr;

    uring_dgram_send_op() noexcept
        : io_uring_op(&do_handler, &do_cqe, &do_prep) {}

    /** Reset and initialize for a new submission.

        Pass `dest_addr_len == 0` for connected-mode datagram sockets
        (the kernel uses the connected peer); otherwise fill
        `dest_addr_storage` with the destination address.
    */
    void prepare(
        std::coroutine_handle<>    handle,
        capy::executor_ref         executor,
        std::error_code*           ec,
        std::size_t*               bytes,
        int                        file_descriptor,
        io_uring_scheduler*        scheduler,
        std::shared_ptr<void>      impl,
        detail::speculative_state* spec,
        buffer_param               buffers,
        socklen_t                  dest_addr_len,
        sockaddr_storage const&    dest_addr_storage,
        int                        flags,
        std::stop_token const&     token) noexcept
    {
        h          = handle;
        ex         = executor;
        ec_out     = ec;
        bytes_out  = bytes;
        fd         = file_descriptor;
        sched_     = scheduler;
        impl_ptr   = std::move(impl);
        spec_state = spec;
        res        = 0;
        cqe_flags  = 0;
        msg_flags  = flags;

        iovec_count = copy_to_iovec(buffers, iovecs);

        msg = {};
        msg.msg_iov    = iovecs;
        msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(iovec_count);
        if (dest_addr_len > 0)
        {
            dest_storage = dest_addr_storage;
            dest_len     = dest_addr_len;
            msg.msg_name    = &dest_storage;
            msg.msg_namelen = dest_addr_len;
        }
        else
        {
            dest_len = 0;
        }
        start(token);
    }

    static void do_prep(io_uring_op* base, ::io_uring_sqe* sqe) noexcept
    {
        auto* self = static_cast<uring_dgram_send_op*>(base);
        ::io_uring_prep_sendmsg(
            sqe, self->fd, &self->msg,
            self->msg_flags | MSG_NOSIGNAL);
    }

    static void do_cqe(
        io_uring_op* base, int res, unsigned flags, op_queue& local) noexcept
    {
        auto* self = static_cast<uring_dgram_send_op*>(base);
        self->res       = res;
        self->cqe_flags = flags;
        local.push(self);
    }

    static void do_handler(
        void* owner, scheduler_op* base,
        std::uint32_t /*bytes*/, std::uint32_t /*error*/) noexcept
    {
        auto* self = static_cast<uring_dgram_send_op*>(base);
        if (coro_drain_if_shutdown(owner, self))
            return;

        if (self->sched_)
            self->sched_->reset_inline_budget();

        // Datagram send: no EOF (a 0-byte send is success).
        decode_io_result(
            self->ec_out,
            self->cancelled.load(std::memory_order_acquire),
            self->res < 0 ? make_err(-self->res) : std::error_code{},
            /*is_read=*/false, /*bytes=*/0, /*empty_buffer=*/false);
        if (self->bytes_out)
            *self->bytes_out = (self->res >= 0)
                ? static_cast<std::size_t>(self->res) : 0;

        if (self->res > 0 && self->spec_state)
        {
            // Kernel signalled readiness — restore speculation.
            self->spec_state->on_async_write_ready();
        }

        coro_resume(self);
    }
};

/** Datagram receive op — connected and unconnected.

    Always uses `IORING_OP_RECVMSG`. In connected mode `msg.msg_name`
    is null. In unconnected mode `msg.msg_name` points at
    `source_storage` and the kernel writes the source address there.

    `res == 0` is success (zero-byte datagrams are valid), NOT EOF.

    The `source_writer` callback lets the concrete socket type
    translate `sockaddr_storage` into `endpoint*` or `local_endpoint*`
    without the op needing to know which family it is.
*/
struct uring_dgram_recv_op : io_uring_op
{
    iovec            iovecs[io_uring_max_iov];
    int              iovec_count = 0;
    msghdr           msg{};
    sockaddr_storage source_storage{};
    socklen_t        source_len = 0;
    int              fd         = -1;
    int              msg_flags  = 0;
    detail::speculative_state* spec_state = nullptr;

    /// Type-erased translator: writes source_storage into the user's
    /// endpoint output via concrete-class-specific conversion.
    void* source_writer_ctx = nullptr;
    void (*source_writer)(
        void*, sockaddr_storage const&, socklen_t) noexcept = nullptr;

    uring_dgram_recv_op() noexcept
        : io_uring_op(&do_handler, &do_cqe, &do_prep) {}

    /** Reset and initialize for a new submission.

        When `source_fn` is non-null, the kernel writes the peer
        address into `source_storage` and `source_fn(source_ctx, ...)`
        is invoked from the handler on success to translate it to
        the user's endpoint output. Connected-mode receivers should
        pass `source_fn = nullptr`.

        A zero-iovec `buffers` argument yields `iovec_count == 0`;
        the caller should push the slot onto `completed_ops_`
        directly (bypassing the kernel) since `recvmsg` would
        otherwise block forever.
    */
    void prepare(
        std::coroutine_handle<>    handle,
        capy::executor_ref         executor,
        std::error_code*           ec,
        std::size_t*               bytes,
        int                        file_descriptor,
        io_uring_scheduler*        scheduler,
        std::shared_ptr<void>      impl,
        detail::speculative_state* spec,
        buffer_param               buffers,
        void*                      source_ctx,
        void (*source_fn)(void*, sockaddr_storage const&, socklen_t) noexcept,
        int                        flags,
        std::stop_token const&     token) noexcept
    {
        h          = handle;
        ex         = executor;
        ec_out     = ec;
        bytes_out  = bytes;
        fd         = file_descriptor;
        sched_     = scheduler;
        impl_ptr   = std::move(impl);
        spec_state = spec;
        res        = 0;
        cqe_flags  = 0;
        msg_flags  = flags;

        iovec_count = copy_to_iovec(buffers, iovecs);

        msg = {};
        // For the zero-iovec bypass path the caller pushes the slot
        // straight onto completed_ops_; source_writer must NOT run in
        // that case (no recvmsg ever happens, source_storage is empty
        // and would clobber the user's endpoint). Arm the writer only
        // when there's a real buffer AND the caller asked for it.
        if (iovec_count > 0 && source_fn)
        {
            msg.msg_iov    = iovecs;
            msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(
                iovec_count);
            source_storage    = {};
            source_len        = sizeof(source_storage);
            msg.msg_name      = &source_storage;
            msg.msg_namelen   = source_len;
            source_writer_ctx = source_ctx;
            source_writer     = source_fn;
        }
        else
        {
            if (iovec_count > 0)
            {
                msg.msg_iov    = iovecs;
                msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(
                    iovec_count);
            }
            source_len        = 0;
            source_writer_ctx = nullptr;
            source_writer     = nullptr;
        }
        start(token);
    }

    static void do_prep(io_uring_op* base, ::io_uring_sqe* sqe) noexcept
    {
        auto* self = static_cast<uring_dgram_recv_op*>(base);
        ::io_uring_prep_recvmsg(
            sqe, self->fd, &self->msg, self->msg_flags);
    }

    static void do_cqe(
        io_uring_op* base, int res, unsigned flags, op_queue& local) noexcept
    {
        auto* self = static_cast<uring_dgram_recv_op*>(base);
        self->res       = res;
        self->cqe_flags = flags;
        // recvmsg writes the actual source addrlen back into msg.msg_namelen.
        self->source_len = self->msg.msg_namelen;
        local.push(self);
    }

    static void do_handler(
        void* owner, scheduler_op* base,
        std::uint32_t /*bytes*/, std::uint32_t /*error*/) noexcept
    {
        auto* self = static_cast<uring_dgram_recv_op*>(base);
        if (coro_drain_if_shutdown(owner, self))
            return;

        if (self->sched_)
            self->sched_->reset_inline_budget();

        // Datagram recv: a 0-byte datagram is success, not EOF — is_read
        // stays false so the shared decode never maps it to end_of_file.
        decode_io_result(
            self->ec_out,
            self->cancelled.load(std::memory_order_acquire),
            self->res < 0 ? make_err(-self->res) : std::error_code{},
            /*is_read=*/false, /*bytes=*/0, /*empty_buffer=*/false);
        if (self->bytes_out)
            *self->bytes_out = (self->res >= 0)
                ? static_cast<std::size_t>(self->res) : 0;

        if (self->res > 0 && self->spec_state)
        {
            // Kernel signalled readiness — restore speculation.
            self->spec_state->on_async_read_ready();
        }

        // Translate source storage into user's endpoint output (only on
        // success and only when the concrete socket type asked for it).
        if (self->source_writer && self->res >= 0)
            self->source_writer(self->source_writer_ctx,
                self->source_storage, self->source_len);

        coro_resume(self);
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IO_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_DGRAM_OPS_HPP
