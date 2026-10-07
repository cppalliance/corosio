//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DESCRIPTOR_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DESCRIPTOR_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/posix_stream_descriptor.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/validate_fd.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>
#include <boost/corosio/native/detail/reactor/reactor_io_core.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op_complete.hpp>
#include <boost/capy/buffers.hpp>

#include <atomic>
#include <coroutine>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

#include <errno.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

/* Reactor-backed implementation of posix_stream_descriptor.

   The register/park/cancel/teardown protocol lives in reactor_io_core.

   The one behavior that is genuinely new: O_NONBLOCK is armed lazily,
   on the first read_some/write_some and never from assign() or wait().
   The flag lives on the shared open file description, so arming it is
   visible to every other holder of that description -- which is why a
   wait()-only user must never trigger it.
*/

namespace boost::corosio::detail {

// ============================================================
// Op types
// ============================================================

/* Descriptor op family.

   Mirrors reactor_stream_ops.hpp. The Acceptor parameter is a
   placeholder: reactor_op is parameterized on both a socket and an
   acceptor impl type, and the shared completion helpers name
   acceptor_impl_ in a branch that a descriptor op never takes but the
   compiler still instantiates. Passing the backend's acceptor type (as
   reactor_dgram_socket_impl already does) keeps those helpers shared
   rather than duplicated here.

   @tparam Traits     Backend traits (epoll_traits, kqueue_traits, ...).
   @tparam Descriptor The concrete descriptor type (forward-declared).
   @tparam Acceptor   Placeholder acceptor type for the op base.
*/

template<class Traits, class Descriptor, class Acceptor>
struct reactor_descriptor_base_op : reactor_op<Descriptor, Acceptor>
{
    void operator()() override;
    void cancel() noexcept override;
};

template<class Traits, class Descriptor, class Acceptor>
struct reactor_descriptor_read_op final
    : reactor_read_op<reactor_descriptor_base_op<Traits, Descriptor, Acceptor>>
{};

template<class Traits, class Descriptor, class Acceptor>
struct reactor_descriptor_write_op final
    : reactor_write_op<
          reactor_descriptor_base_op<Traits, Descriptor, Acceptor>,
          typename Traits::descriptor_write_policy>
{};

template<class Traits, class Descriptor, class Acceptor>
struct reactor_descriptor_wait_op final
    : reactor_wait_op<reactor_descriptor_base_op<Traits, Descriptor, Acceptor>>
{
    using base_type = reactor_wait_op<
        reactor_descriptor_base_op<Traits, Descriptor, Acceptor>>;

    /** Probe like reactor_wait_op::probe, but name a code for an error
        wait that sees POLLERR or POLLHUP. A non-socket has no SO_ERROR
        to consult, and success would claim no error condition exists.
    */
    static bool probe(int fd, std::uint32_t event, int& err) noexcept
    {
        if (event != reactor_event_error || fd < 0)
            return base_type::probe(fd, event, err);

        pollfd pfd{};
        pfd.fd     = fd;
        pfd.events = POLLPRI;
        int r;
        do
        {
            r = ::poll(&pfd, 1, 0);
        }
        while (r < 0 && errno == EINTR);
        if (r < 0)
        {
            err = (errno == EAGAIN || errno == EWOULDBLOCK) ? ENOMEM : errno;
            return true;
        }
        if (r == 0)
            return false;
        if (pfd.revents & POLLNVAL)
            err = EBADF;
        else if (pfd.revents & (POLLERR | POLLHUP))
            err = EIO;
        else if (is_fifo(fd))
            // Darwin reports POLLPRI on a pipe that merely holds data;
            // a FIFO has no exceptional condition to signal.
            return false;
        return true;
    }

    static bool is_fifo(int fd) noexcept
    {
        struct stat st;
        return ::fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode);
    }

    void perform_io() noexcept override
    {
        int err = 0;
        if (probe(this->fd, this->wait_event, err))
            this->complete(err, 0);
        else
            this->complete(EAGAIN, 0);
    }

    void operator()() override;
};

// --- Deferred implementations (instantiated when Descriptor is complete) ---

template<class Traits, class Descriptor, class Acceptor>
void
reactor_descriptor_base_op<Traits, Descriptor, Acceptor>::operator()()
{
    complete_io_op(*this);
}

template<class Traits, class Descriptor, class Acceptor>
void
reactor_descriptor_base_op<Traits, Descriptor, Acceptor>::cancel() noexcept
{
    // A descriptor op is only ever started against a descriptor impl, so
    // the acceptor arm of the stream op's cancel() has no counterpart.
    if (this->socket_impl_)
        this->socket_impl_->cancel_single_op(*this);
    else
        this->request_cancel();
}

template<class Traits, class Descriptor, class Acceptor>
void
reactor_descriptor_wait_op<Traits, Descriptor, Acceptor>::operator()()
{
    complete_wait_op(*this);
}

// ============================================================
// Descriptor implementation
// ============================================================

/** CRTP base for reactor-backed posix_stream_descriptor implementations.

    Holds the adopted descriptor, its reactor registration state, and
    the five op slots (read, write, and one wait per direction).

    @tparam Derived  The named final class (CRTP self).
    @tparam Traits   Backend traits (epoll_traits, kqueue_traits, ...).
    @tparam Service  The backend's descriptor service type.
    @tparam Acceptor Placeholder acceptor type for the op base.
*/
template<class Derived, class Traits, class Service, class Acceptor>
class reactor_descriptor
    : public posix_stream_descriptor::implementation
    , public reactor_io_core<Derived, Service, typename Traits::desc_state_type>
    , public intrusive_list<Derived>::node
{
    using base_op  = reactor_descriptor_base_op<Traits, Derived, Acceptor>;
    using read_op  = reactor_descriptor_read_op<Traits, Derived, Acceptor>;
    using write_op = reactor_descriptor_write_op<Traits, Derived, Acceptor>;
    using wait_op  = reactor_descriptor_wait_op<Traits, Derived, Acceptor>;

    using core_type =
        reactor_io_core<Derived, Service, typename Traits::desc_state_type>;
    friend core_type;

protected:
    // NOLINTNEXTLINE(bugprone-crtp-constructor-accessibility)
    explicit reactor_descriptor(Service& svc) noexcept : core_type(svc) {}

    using core_type::svc_;

public:
    ~reactor_descriptor() override = default;

    using core_type::desc_state_;

    // --- Virtual method overrides ---

    std::coroutine_handle<> read_some(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes_out) override
    {
        return do_read_some(h, ex, param, token, ec, bytes_out);
    }

    std::coroutine_handle<> write_some(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes_out) override
    {
        return do_write_some(h, ex, param, token, ec, bytes_out);
    }

    std::coroutine_handle<> wait(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        wait_type w,
        std::stop_token token,
        std::error_code* ec) override
    {
        return do_wait(h, ex, w, token, ec);
    }

    native_handle_type native_handle() const noexcept override
    {
        return fd_;
    }

    native_handle_type release_descriptor() noexcept override;

    void cancel() noexcept override
    {
        this->cancel_all();
    }

    /// Recycle into the owning service's pool at zero references.
    void retire() noexcept override
    {
#if !defined(NDEBUG)
        poison();
#endif
        svc_.state_->pool_.recycle(static_cast<Derived*>(this));
    }

    // --- Service-facing (non-virtual) ---

    /** Reset descriptor state for recycling.

        Re-initializes every field it owns rather than trusting
        `close_descriptor()`'s prior writes; see
        reactor_basic_socket::reuse().

        @pre refs_ == 0, fd closed and deregistered, no op in flight.
    */
    void reuse() noexcept
    {
        fd_ = -1;
        nonblocking_.store(false, std::memory_order_relaxed);
        this->reset_desc_state();
    }

#if !defined(NDEBUG)
    /// Poison the fields `reuse()` re-initializes; see
    /// reactor_basic_socket::poison().
    void poison() noexcept
    {
        std::memset(static_cast<void*>(&fd_), 0xDB, sizeof(fd_));
        this->poison_desc_state();
    }
#endif

    /** Adopt the fd, initialize descriptor state, and register it.

        @param fd The descriptor to adopt.

        @return The error if the reactor rejects the descriptor, in
        which case the implementation is left closed and the caller
        retains ownership of @a fd; otherwise a default constructed
        error code.
    */
    std::error_code init_and_register(int fd) noexcept;

    /// Close the descriptor and cancel pending operations.
    void close_descriptor() noexcept;

private:
    /** Arm O_NONBLOCK, once, before the first speculative syscall.

        Reports an errno rather than an error_code because that is what
        the op result model records; the round trip is lossless here
        because fcntl only fails with codes make_err passes through.
    */
    int arm_nonblocking() noexcept
    {
        // Relaxed: ensure_nonblocking is idempotent, so a duplicate
        // fcntl from a racing first read and write is harmless.
        if (nonblocking_.load(std::memory_order_relaxed))
            return 0;
        if (auto ec = ensure_nonblocking(fd_))
            return ec.value();
        nonblocking_.store(true, std::memory_order_relaxed);
        return 0;
    }

    std::coroutine_handle<> do_read_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token const&,
        std::error_code*,
        std::size_t*);

    std::coroutine_handle<> do_write_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token const&,
        std::error_code*,
        std::size_t*);

    std::coroutine_handle<> do_wait(
        std::coroutine_handle<>,
        capy::executor_ref,
        wait_type,
        std::stop_token const&,
        std::error_code*);

    /// Apply @a fn to each of the five op slots.
    template<class Fn>
    void for_each_op(Fn fn) noexcept
    {
        fn(rd_);
        fn(wr_);
        fn(wait_rd_);
        fn(wait_wr_);
        fn(wait_er_);
    }

    /// Apply @a fn to each op and the descriptor_state slot it parks in.
    template<class Fn>
    void for_each_desc_entry(Fn fn) noexcept
    {
        fn(rd_, desc_state_.read_op);
        fn(wr_, desc_state_.write_op);
        fn(wait_rd_, desc_state_.wait_read_op);
        fn(wait_wr_, desc_state_.wait_write_op);
        fn(wait_er_, desc_state_.wait_error_op);
    }

    /// Sweep every op slot, then drop the reactor registration.
    void quiesce() noexcept;

    reactor_op_base** op_to_desc_slot(base_op& op) noexcept;

    int fd_           = -1;
    std::atomic<bool> nonblocking_{false};

    read_op rd_;
    write_op wr_;
    wait_op wait_rd_;
    wait_op wait_wr_;
    wait_op wait_er_;
};

// ============================================================
// Registration and teardown
// ============================================================

template<class Derived, class Traits, class Service, class Acceptor>
std::error_code
reactor_descriptor<Derived, Traits, Service, Acceptor>::init_and_register(
    int fd) noexcept
{
    fd_ = fd;
    if (auto ec = this->register_fd(fd))
    {
        fd_ = -1;
        return ec;
    }
    return {};
}

template<class Derived, class Traits, class Service, class Acceptor>
void
reactor_descriptor<Derived, Traits, Service, Acceptor>::quiesce() noexcept
{
    this->abandon_all();
    // The next adopted fd starts from an unknown flag state.
    nonblocking_.store(false, std::memory_order_relaxed);
}

template<class Derived, class Traits, class Service, class Acceptor>
void
reactor_descriptor<Derived, Traits, Service, Acceptor>::
    close_descriptor() noexcept
{
    quiesce();

    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

template<class Derived, class Traits, class Service, class Acceptor>
native_handle_type
reactor_descriptor<Derived, Traits, Service, Acceptor>::
    release_descriptor() noexcept
{
    quiesce();

    // Do NOT close -- the caller takes ownership.
    native_handle_type released = fd_;
    fd_                         = -1;
    return released;
}

// ============================================================
// Op slot lookup
// ============================================================

template<class Derived, class Traits, class Service, class Acceptor>
reactor_op_base**
reactor_descriptor<Derived, Traits, Service, Acceptor>::op_to_desc_slot(
    base_op& op) noexcept
{
    if (&op == static_cast<void*>(&rd_))
        return &desc_state_.read_op;
    if (&op == static_cast<void*>(&wr_))
        return &desc_state_.write_op;
    if (&op == static_cast<void*>(&wait_rd_))
        return &desc_state_.wait_read_op;
    if (&op == static_cast<void*>(&wait_wr_))
        return &desc_state_.wait_write_op;
    if (&op == static_cast<void*>(&wait_er_))
        return &desc_state_.wait_error_op;
    return nullptr;
}

// ============================================================
// I/O dispatch
// ============================================================

template<class Derived, class Traits, class Service, class Acceptor>
std::coroutine_handle<>
reactor_descriptor<Derived, Traits, Service, Acceptor>::do_read_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param param,
    std::stop_token const& token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = rd_;
    op.reset();
    op.h         = h;
    op.ex        = ex;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;

    // Closed-object contract: complete with bad_file_descriptor without
    // touching the kernel or the unregistered descriptor state.
    if (fd_ < 0)
    {
        op.start(token, static_cast<Derived*>(this));
        op.object_ref_ = detail::object_ref(this);
        op.complete(EBADF, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    capy::mutable_buffer bufs[read_op::max_buffers];
    op.iovec_count =
        static_cast<int>(param.copy_to(bufs, read_op::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.empty_buffer_read = true;
        op.start(token, static_cast<Derived*>(this));
        op.object_ref_ = detail::object_ref(this);
        op.complete(0, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    // The first transferring operation is what arms O_NONBLOCK; assign()
    // and wait() never do.
    if (int const nerr = arm_nonblocking())
    {
        op.start(token, static_cast<Derived*>(this));
        op.object_ref_ = detail::object_ref(this);
        op.complete(nerr, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len  = bufs[i].size();
    }

    // Speculative read; the single-buffer case uses read() so the kernel
    // skips the readv iov_iter setup.
    ssize_t n;
    if (op.iovec_count == 1)
    {
        do
        {
            n = ::read(fd_, bufs[0].data(), bufs[0].size());
        }
        while (n < 0 && errno == EINTR);
    }
    else
    {
        do
        {
            n = ::readv(fd_, op.iovecs, op.iovec_count);
        }
        while (n < 0 && errno == EINTR);
    }

    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
    {
        int err    = (n < 0) ? errno : 0;
        auto bytes = (n > 0) ? static_cast<std::size_t>(n) : std::size_t(0);

        if (svc_.scheduler().try_consume_inline_budget())
        {
            if (err)
                *ec = make_err(err);
            else if (n == 0)
                *ec = capy::error::eof;
            else
                *ec = {};
            *bytes_out = bytes;
            op.cont.h  = h;
            return dispatch_coro(ex, op.cont);
        }
        op.start(token, static_cast<Derived*>(this));
        op.object_ref_ = detail::object_ref(this);
        op.complete(err, bytes);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    // EAGAIN — register with reactor
    op.fd = fd_;
    op.start(token, static_cast<Derived*>(this));
    op.object_ref_ = detail::object_ref(this);

    this->register_op(op, desc_state_.read_op, desc_state_.read_ready);
    return std::noop_coroutine();
}

template<class Derived, class Traits, class Service, class Acceptor>
std::coroutine_handle<>
reactor_descriptor<Derived, Traits, Service, Acceptor>::do_write_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param param,
    std::stop_token const& token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = wr_;
    op.reset();
    op.h         = h;
    op.ex        = ex;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;

    if (fd_ < 0)
    {
        op.start(token, static_cast<Derived*>(this));
        op.object_ref_ = detail::object_ref(this);
        op.complete(EBADF, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    capy::mutable_buffer bufs[write_op::max_buffers];
    op.iovec_count =
        static_cast<int>(param.copy_to(bufs, write_op::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.start(token, static_cast<Derived*>(this));
        op.object_ref_ = detail::object_ref(this);
        op.complete(0, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    if (int const nerr = arm_nonblocking())
    {
        op.start(token, static_cast<Derived*>(this));
        op.object_ref_ = detail::object_ref(this);
        op.complete(nerr, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len  = bufs[i].size();
    }

    // Speculative write; the single-buffer case skips the iov_iter setup.
    ssize_t n;
    if (op.iovec_count == 1)
    {
        n = write_op::write_policy::write_one(
            fd_, bufs[0].data(), bufs[0].size());
    }
    else
    {
        n = write_op::write_policy::write(fd_, op.iovecs, op.iovec_count);
    }

    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
    {
        int err    = (n < 0) ? errno : 0;
        auto bytes = (n > 0) ? static_cast<std::size_t>(n) : std::size_t(0);

        if (svc_.scheduler().try_consume_inline_budget())
        {
            *ec        = err ? make_err(err) : std::error_code{};
            *bytes_out = bytes;
            op.cont.h  = h;
            return dispatch_coro(ex, op.cont);
        }
        op.start(token, static_cast<Derived*>(this));
        op.object_ref_ = detail::object_ref(this);
        op.complete(err, bytes);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    // EAGAIN — register with reactor
    op.fd = fd_;
    op.start(token, static_cast<Derived*>(this));
    op.object_ref_ = detail::object_ref(this);

    this->register_op(op, desc_state_.write_op, desc_state_.write_ready, true);
    return std::noop_coroutine();
}

template<class Derived, class Traits, class Service, class Acceptor>
std::coroutine_handle<>
reactor_descriptor<Derived, Traits, Service, Acceptor>::do_wait(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    wait_type w,
    std::stop_token const& token,
    std::error_code* ec)
{
    // Pick refs up-front to avoid duplicating the register_op call.
    wait_op* op_ptr;
    reactor_op_base** desc_slot_ptr;
    std::uint32_t event;

    if (w == wait_type::read)
    {
        op_ptr        = &wait_rd_;
        desc_slot_ptr = &desc_state_.wait_read_op;
        event         = reactor_event_read;
    }
    else if (w == wait_type::write)
    {
        op_ptr        = &wait_wr_;
        desc_slot_ptr = &desc_state_.wait_write_op;
        event         = reactor_event_write;
    }
    else // wait_type::error
    {
        op_ptr        = &wait_er_;
        desc_slot_ptr = &desc_state_.wait_error_op;
        event         = reactor_event_error;
    }

    auto& op = *op_ptr;

    // Speculative probe: an edge-triggered reactor cannot report a
    // condition that already holds, so a wait initiated on an already
    // ready descriptor would otherwise park forever. No syscall here
    // modifies the descriptor -- in particular O_NONBLOCK is untouched.
    int perr = 0;
    if (wait_op::probe(fd_, event, perr))
    {
        if (svc_.scheduler().try_consume_inline_budget())
        {
            *ec       = perr ? make_err(perr) : std::error_code{};
            op.cont.h = h;
            return dispatch_coro(ex, op.cont);
        }
        op.reset();
        op.wait_event = event;
        op.h          = h;
        op.ex         = ex;
        op.ec_out     = ec;
        op.fd         = fd_;
        op.start(token, static_cast<Derived*>(this));
        op.object_ref_ = detail::object_ref(this);
        op.complete(perr, 0);
        svc_.post(&op);
        return std::noop_coroutine();
    }

    op.reset();
    op.wait_event = event;
    op.h          = h;
    op.ex         = ex;
    op.ec_out     = ec;
    op.fd         = fd_;
    op.start(token, static_cast<Derived*>(this));
    op.object_ref_ = detail::object_ref(this);

    // Force register_op's ready path so the wait op re-probes under the
    // descriptor mutex before parking. A stale write_ready latched at
    // registration would otherwise report a full pipe as writable.
    bool force_probe = true;
    this->register_op(
        op, *desc_slot_ptr, force_probe, event == reactor_event_write);
    return std::noop_coroutine();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DESCRIPTOR_HPP
