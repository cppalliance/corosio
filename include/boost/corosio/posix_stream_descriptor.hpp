//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_POSIX_STREAM_DESCRIPTOR_HPP
#define BOOST_COROSIO_POSIX_STREAM_DESCRIPTOR_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX || defined(BOOST_COROSIO_MRDOCS)

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/detail/op_base.hpp>
#include <boost/corosio/error.hpp>
#include <boost/corosio/io/io_stream.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/concept/executor.hpp>

#include <concepts>
#include <boost/capy/continuation.hpp>

#include <coroutine>
#include <stop_token>
#include <system_error>
#include <type_traits>

/* Adoption of an already-open pollable POSIX descriptor.

   The two contract points that are not obvious from the
   declarations:

   assign() requires a closed object and never touches an open one.
   Every failure, validation or kernel refusal, leaves the object
   closed and the fd with the caller.

   On the reactor backends O_NONBLOCK is applied lazily, at the first
   read_some/write_some, and never restored; io_uring never touches
   it. A wait()-only user never triggers it, which is what makes
   adopting STDIN_FILENO safe: flipping the flag would change the
   parent shell's terminal, because the flag lives on the shared open
   file description, not on the descriptor.
*/

namespace boost::corosio {

/** Drives an already-open POSIX descriptor from an `io_context`.

    Wraps an already-open pollable file descriptor and drives it
    from the `io_context`. The kinds in scope are character devices,
    `inotify`, `eventfd`, `timerfd`, `pidfd`, pipes, ttys, and socket
    kinds corosio does not otherwise wrap. The descriptor must come
    from the caller; this type never creates one.

    The type name is deliberately platform-qualified. Portability
    comes from the interfaces it implements, not from the name. A
    `posix_stream_descriptor` is an @ref io_stream. `capy::read`,
    `capy::write`, other `capy::Stream`-constrained algorithms and
    TLS layering therefore work on it exactly as they do on a
    socket.

    @par Ownership
    `assign()` takes ownership and `close()` closes the
    descriptor. To integrate with a library that owns the fd, adopt
    a `dup()` of it: readiness lives on the open file description,
    which both descriptors share.

    @par Descriptor Flags
    `assign()` and `wait()` never modify the descriptor on any
    backend. On epoll, kqueue and select the first `read_some()` or
    `write_some()` sets `O_NONBLOCK` and never restores it. On
    io_uring nothing is ever modified. A transfer the kernel cannot
    complete through its internal poll waits in a kernel worker
    thread. Cancellation reaches it only if the driver's wait is
    interruptible. The flag lives on the shared
    open file description, so restoring it would race every other
    holder. A
    `dup()` is no escape: the duplicate shares that same description,
    so the flag change reaches the other holder anyway. When another
    party owns the descriptor and cannot tolerate `O_NONBLOCK`, use
    `wait()` -- which never modifies the descriptor -- and do the I/O
    yourself.

    @par Rejected Descriptors
    Regular files, block devices, and directories are rejected with
    `errc::operation_not_supported`. @ref stream_file and
    @ref random_access_file adopt regular files and block devices. A
    directory is adoptable by no corosio type. A character device
    the I/O backend cannot watch, such as `/dev/null`, is adopted on
    every I/O backend. On epoll, kqueue, and io_uring, an operation
    on it that would have to wait for readiness completes with
    `errc::operation_not_supported`. The exception is a transfer on
    io_uring when the descriptor is blocking: it waits in a kernel
    worker thread instead. On select, the device is always ready for
    reading and writing. Its `wait(wait_type::error)` waits until
    cancelled, except on macOS, where it completes at once with an
    error. On select, a descriptor at or above `FD_SETSIZE` is
    rejected with `errc::too_many_files_open`. Where a kernel refusal
    surfaces depends on the backend. The epoll and kqueue backends
    register the descriptor during `assign()`, so a refusal fails
    there. What remains to refuse is resource exhaustion (`ENOMEM`,
    `ENOSPC`).
    kqueue watches writes only once a write-direction operation first
    has to wait. A descriptor that refuses write watching is still
    adopted, and such a write or `wait(wait_type::write)` completes
    with the kernel's refusal. The io_uring backend has no adopt-time
    registration, so `assign()` succeeds and takes ownership. The
    refusal appears at the first `read_some()` or `write_some()`.
    select registers nothing with the kernel, so it has no refusal to
    report.

    @par Signals
    Writing to a descriptor whose peer has closed raises `SIGPIPE`
    in the default disposition -- unlike the socket types, which
    suppress it. `MSG_NOSIGNAL` is a `send()` flag with no `writev`
    equivalent, and `SO_NOSIGPIPE` is a socket option, so neither
    applies to an arbitrary descriptor. Callers must install
    `SIG_IGN` for `SIGPIPE` if that is not already the process's
    disposition.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. A descriptor must not have concurrent
    operations of the same type (e.g. two simultaneous reads). One
    read and one write may be in flight simultaneously.

    @see io_stream, stream_file, wait_type
*/
class BOOST_COROSIO_DECL posix_stream_descriptor : public io_stream
{
public:
    /** Define backend hooks for descriptor operations.

        Platform backends (epoll, kqueue, select, io_uring) derive
        from this to implement descriptor I/O.
    */
    struct implementation : io_stream::implementation
    {
        /** Initiate an asynchronous wait for descriptor readiness.

            Completes when the descriptor becomes ready in the
            given direction, or an error condition is reported. No
            bytes are transferred and no descriptor flag is changed.

            @param cont Continuation to resume on completion; it lives in
                the awaiting frame until then.
            @param ex Executor for dispatching the completion.
            @param w The direction to wait on.
            @param token Stop token for cancellation.
            @param ec Output error code.
            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> wait(
            capy::continuation& cont,
            capy::executor_ref ex,
            wait_type w,
            std::stop_token token,
            std::error_code* ec) = 0;

        /// Return the platform descriptor, or -1 when not open.
        virtual native_handle_type native_handle() const noexcept = 0;

        /** Release ownership of the native descriptor.

            Stops tracking the descriptor and cancels its pending
            operations, without closing it. The caller takes
            ownership.

            @return The native descriptor.
        */
        virtual native_handle_type release_descriptor() noexcept = 0;

        /** Request cancellation of pending asynchronous operations.

            All outstanding operations complete with a code that
            compares equal to `capy::cond::canceled`.
        */
        virtual void cancel() noexcept = 0;
    };

    /// Represent the awaitable returned by @ref wait.
    struct wait_awaitable : detail::void_op_base<wait_awaitable>
    {
    private:
        friend posix_stream_descriptor;

        wait_awaitable(posix_stream_descriptor& d, wait_type w) noexcept
            : d_(d)
            , w_(w)
        {
        }

        friend detail::void_op_base<wait_awaitable>;

        posix_stream_descriptor& d_;
        wait_type w_;

        std::coroutine_handle<>
        dispatch(capy::continuation& cont, capy::executor_ref ex) const
        {
            return d_.get().wait(cont, ex, w_, token_, &ec_);
        }
    };

    /** Closes the descriptor if open, cancelling pending operations. */
    ~posix_stream_descriptor() override;

    /** Construct from an execution context.

        @param ctx The execution context that owns this object.
    */
    explicit posix_stream_descriptor(capy::execution_context& ctx);

    /** Construct from an executor.

        The overload excludes `posix_stream_descriptor` itself so that it
        cannot displace the move constructor.

        @tparam Ex A type satisfying `capy::Executor`.
        @param ex The executor whose context owns this object.
    */
    template<class Ex>
        requires(!std::same_as<
                    std::remove_cvref_t<Ex>,
                    posix_stream_descriptor>) &&
        capy::Executor<Ex>
    explicit posix_stream_descriptor(Ex const& ex)
        : posix_stream_descriptor(ex.context())
    {
    }

    /** Transfer ownership of the descriptor from @p other.

        After the move, @p other is in a moved-from state and may only
        be destroyed or assigned to.

        @param other The object to move from.
        @pre No awaitables returned by @p other's methods exist.
    */
    posix_stream_descriptor(posix_stream_descriptor&& other) noexcept
        : io_object(std::move(other))
    {
    }

    /** Close any held descriptor and transfer ownership from @p other.

        After the move, @p other is in a moved-from state and may only
        be destroyed or assigned to.

        @param other The object to move from.
        @return `*this`.
        @pre No awaitables returned by either object's methods exist.
    */
    posix_stream_descriptor& operator=(posix_stream_descriptor&& other) noexcept
    {
        io_object::operator=(std::move(other));
        return *this;
    }

    /// Copy construction is disabled; the descriptor is uniquely owned.
    posix_stream_descriptor(posix_stream_descriptor const&) = delete;
    /// Copy assignment is disabled; the descriptor is uniquely owned.
    posix_stream_descriptor& operator=(posix_stream_descriptor const&) = delete;

    /** Adopt an existing native descriptor.

        The object must be closed. To replace a held descriptor,
        `close()` or `release()` it first. On success the object takes
        ownership and @p fd is closed by `close()` or the destructor.

        No descriptor flag is modified here, `O_NONBLOCK` included.

        @param fd The native descriptor to adopt.

        @return `error::already_open` if this object is open.
            `errc::bad_file_descriptor` when @p fd is negative or
            closed. `errc::operation_not_supported` when @p fd names
            a regular file, block device, or directory.
            `errc::too_many_files_open` on select when @p fd is at or
            above `FD_SETSIZE`. Otherwise the error the system
            reported, or an empty code.

        @par Exception Safety
        Throws nothing. On failure the object is unchanged and @p fd
        stays with the caller.

        @see release
    */
    [[nodiscard]] std::error_code assign(native_handle_type fd) noexcept;

    /** Release ownership of the native descriptor.

        The object becomes not-open and pending operations are
        cancelled. The caller is responsible for closing the result.

        @return The native descriptor.

        @throws std::system_error `errc::bad_file_descriptor` if the
            object is not open.

        @post `is_open() == false`
    */
    native_handle_type release();

    /** Close the descriptor.

        Pending operations complete with a code that compares equal
        to `capy::cond::canceled`. Does nothing when not open.
    */
    void close() noexcept;

    /** Check whether a descriptor is held.

        @return `true` if a descriptor is held.
    */
    bool is_open() const noexcept
    {
        return h_ && get().native_handle() >= 0;
    }

    /** Get the native descriptor.

        @return The native descriptor, or -1 when not open.
    */
    native_handle_type native_handle() const noexcept;

    /** Cancel pending asynchronous operations.

        Outstanding operations complete with a code that compares
        equal to `capy::cond::canceled`.
    */
    void cancel() noexcept;

    /** Wait for readiness without transferring bytes.

        Never reads, writes or modifies the descriptor -- including
        its flags -- which is what makes it safe on a descriptor
        another library owns.

        @param w The direction to wait on.

        @return An awaitable yielding `capy::io_result<>`. Yields
            `errc::bad_file_descriptor` when not open.

        @par Example
        @par !example wait

        @see wait_type
    */
    [[nodiscard]] wait_awaitable wait(wait_type w)
    {
        return wait_awaitable(*this, w);
    }

protected:
    /// Default-construct (for derived types that initialize `io_object` directly).
    posix_stream_descriptor() noexcept = default;

    /** Construct from a handle.

        @param h The handle this object takes ownership of.
    */
    explicit posix_stream_descriptor(handle h) noexcept
        : io_object(std::move(h))
    {
    }

private:
    /// Return the implementation downcast to this type's interface.
    implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX || BOOST_COROSIO_MRDOCS

#endif
