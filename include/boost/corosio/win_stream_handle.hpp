//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_WIN_STREAM_HANDLE_HPP
#define BOOST_COROSIO_WIN_STREAM_HANDLE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP || defined(BOOST_COROSIO_MRDOCS)

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/io/io_stream.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/concept/executor.hpp>

#include <concepts>
#include <coroutine>
#include <stop_token>
#include <system_error>
#include <type_traits>

namespace boost::corosio {
/** Drives an already-open overlapped Windows handle from an `io_context`.

    Wraps a handle with an implicit position: the server or client
    end of a named pipe opened with `FILE_FLAG_OVERLAPPED`, a COM
    port, or a mailslot. The handle must come from the caller; this
    type never creates one.

    The type name is deliberately platform-qualified. Portability
    comes from the interfaces it implements, not from the name. A
    `win_stream_handle` is an @ref io_stream, so `capy::read`,
    `capy::write`, other `capy::Stream`-constrained algorithms and
    TLS layering work on it exactly as they do on a socket.

    There is no `wait()`. IOCP has no readiness primitive for
    arbitrary handles, which is the reason this type is not a
    portable one.

    @par Ownership
    `assign()` takes ownership and `close()` closes the handle.
    `release()` detaches it from this context's completion port and
    hands it back. It throws and keeps the handle while an operation
    is still in flight, or if Windows refuses the detach.

    While the handle is bound to a completion port, every overlapped
    call on it queues a packet to that port. Do not issue your own
    overlapped I/O on it, such as `ConnectNamedPipe`, `WaitCommEvent`,
    or `DeviceIoControl`. The exception is a call whose `OVERLAPPED`
    has the low-order bit of `hEvent` set, which suppresses the
    packet. Connect a pipe server before `assign()`.

    @par Rejected Handles
    Console handles, sockets, disk files, and directories are rejected
    with `errc::operation_not_supported`. So are handles opened without
    `FILE_FLAG_OVERLAPPED`, including both ends of an anonymous
    `CreatePipe` pipe, and handles already in
    skip-completion-port-on-success mode. A named pipe created with
    `FILE_FLAG_OVERLAPPED` is the substitute for `CreatePipe`;
    @ref stream_file adopts disk files. A handle already bound to
    another completion port fails with `errc::invalid_argument`.

    @par Transfers
    Each `read_some()` and `write_some()` transfers at most the first
    non-empty buffer of the sequence. On a message-mode pipe, a
    message longer than the buffer is returned across successive
    reads. A read that finds the peer closed completes with
    `capy::error::eof`; a write completes with `errc::broken_pipe`.
    On a message-mode pipe, a zero-length message reads as end of file, and
    a zero-length write sends nothing.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. One read and one write may be in flight
    simultaneously; two reads (or two writes) may not.

    @par Example
    @par !example win_stream_handle

    @see io_stream, stream_file, win_random_access_handle
*/
class BOOST_COROSIO_DECL win_stream_handle : public io_stream
{
public:
    /** Define backend hooks for handle operations.

        The IOCP backend derives from this to implement handle I/O.
    */
    struct implementation : io_stream::implementation
    {
        /// Return the platform handle, or `INVALID_HANDLE_VALUE` when not open.
        virtual native_handle_type native_handle() const noexcept = 0;

        /** Release ownership of the native handle.

            Cancels pending operations and detaches the handle from
            the completion port without closing it. The caller takes
            ownership.

            @return The native handle.

            @throws std::system_error `errc::device_or_resource_busy` if
                an operation is still in flight, or
                `errc::operation_not_supported` if the handle cannot be
                detached. The handle stays owned on throw.
        */
        virtual native_handle_type release_handle() = 0;

        /** Request cancellation of pending asynchronous operations.

            All outstanding operations complete with a code that
            compares equal to `capy::cond::canceled`.
        */
        virtual void cancel() noexcept = 0;
    };

    /** Closes the handle if open, cancelling pending operations. */
    ~win_stream_handle() override;

    /** Construct from an execution context.

        @param ctx The execution context that owns this object.
    */
    explicit win_stream_handle(capy::execution_context& ctx);

    /** Construct from an executor.

        The overload excludes `win_stream_handle` itself so that it
        cannot displace the move constructor.

        @tparam Ex A type satisfying `capy::Executor`.
        @param ex The executor whose context owns this object.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, win_stream_handle>) &&
        capy::Executor<Ex>
    explicit win_stream_handle(Ex const& ex) : win_stream_handle(ex.context())
    {
    }

    /** Transfer ownership of the handle from @p other.

        After the move, @p other is in a moved-from state and may only
        be destroyed or assigned to.

        @param other The object to move from.
        @pre No awaitables returned by @p other's methods exist.
    */
    win_stream_handle(win_stream_handle&& other) noexcept
        : io_object(std::move(other))
    {
    }

    /** Close any held handle and transfer ownership from @p other.

        After the move, @p other is in a moved-from state and may only
        be destroyed or assigned to.

        @param other The object to move from.
        @return `*this`.
        @pre No awaitables returned by either object's methods exist.
    */
    win_stream_handle& operator=(win_stream_handle&& other) noexcept
    {
        io_object::operator=(std::move(other));
        return *this;
    }

    /// Copy construction is disabled; the handle is uniquely owned.
    win_stream_handle(win_stream_handle const&) = delete;
    /// Copy assignment is disabled; the handle is uniquely owned.
    win_stream_handle& operator=(win_stream_handle const&) = delete;

    /** Adopt an existing overlapped handle.

        @param h The native handle to adopt.

        @return `error::already_open` if this object is open.
            `errc::invalid_argument` when @p h is bound to another
            completion port. `errc::bad_file_descriptor` when @p h is
            null, invalid, or closed. `errc::operation_not_supported`
            when @p h is a console, a socket, a synchronous-mode
            handle, a handle in skip-completion-port-on-success mode,
            a disk file, or a directory. It is also returned when the
            handle's I/O mode cannot be queried. Otherwise the error
            the system reported, or an empty code.

        @par Exception Safety
        Throws nothing. On failure the object is unchanged and @p h
        stays with the caller.

        @see release
    */
    [[nodiscard]] std::error_code assign(native_handle_type h) noexcept;

    /** Release ownership of the native handle.

        Pending operations are cancelled first. If one is still in
        flight, the object keeps the handle and this throws. The same
        happens when Windows refuses to detach the handle from the
        execution context's completion port. Call `release()` again
        once the cancelled operations have completed. Detaching
        requires Windows 8.1 or later. On success the object becomes
        not-open and the caller is responsible for closing the
        result.

        @return The native handle.

        @throws std::system_error `errc::bad_file_descriptor` if the
            object is not open; `errc::device_or_resource_busy` if an
            operation is still in flight; `errc::operation_not_supported`
            if the handle cannot be detached.
    */
    native_handle_type release();

    /** Close the handle.

        Pending operations complete with a code that compares equal
        to `capy::cond::canceled`. Does nothing when not open.
    */
    void close() noexcept;

    /** Check whether a handle is held.

        @return `true` if a handle is held.
    */
    bool is_open() const noexcept
    {
        return h_ && get().native_handle() != ~native_handle_type{};
    }

    /** Get the native handle.

        @return The native handle, or `INVALID_HANDLE_VALUE` when not open.
    */
    native_handle_type native_handle() const noexcept;

    /** Cancel pending asynchronous operations.

        Outstanding operations complete with a code that compares
        equal to `capy::cond::canceled`.
    */
    void cancel() noexcept;

protected:
    /// Default-construct (for derived types that initialize `io_object` directly).
    win_stream_handle() noexcept = default;

    /** Construct from a handle.

        @param h The handle this object takes ownership of.
    */
    explicit win_stream_handle(handle h) noexcept : io_object(std::move(h)) {}

private:
    /// Return the implementation downcast to this type's interface.
    implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP || BOOST_COROSIO_MRDOCS

#endif
