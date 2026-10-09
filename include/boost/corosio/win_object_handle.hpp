//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_WIN_OBJECT_HANDLE_HPP
#define BOOST_COROSIO_WIN_OBJECT_HANDLE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP || defined(BOOST_COROSIO_MRDOCS)

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/detail/op_base.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/concept/executor.hpp>

#include <concepts>
#include <coroutine>
#include <stop_token>
#include <system_error>
#include <type_traits>

namespace boost::corosio {

/** Waits on an already-open Windows kernel object from an `io_context`.

    Wraps a waitable handle: a process or thread, an event, a
    semaphore, a waitable timer, or a job. A coroutine can await its
    signaled state without blocking a thread. The handle must come
    from the caller and must carry `SYNCHRONIZE` access.

    Waits are carried by the Windows thread pool (one
    `CreateThreadpoolWait` object per `win_object_handle`).
    Completions are delivered through the `io_context`, so the
    awaiting coroutine resumes on its executor as usual.

    @par Rejected Handles
    Only processes, threads, events, semaphores, waitable timers and
    jobs are accepted; any other object type is rejected with
    `errc::operation_not_supported`. Mutexes are excluded because a
    satisfied mutex wait acquires the mutex on a pool thread the
    resuming coroutine does not own. File objects are excluded
    because one signals on any I/O completion. Console input and
    directory change notification handles are file objects, so they
    are rejected too. Pseudo-handles such as
    `GetCurrentThread()` and handles without `SYNCHRONIZE` access
    are rejected the same way. Every handle is rejected on an
    `io_context` whose locking mode is `locking_mode::unsafe`,
    because the wait completes from a thread-pool thread.

    @par Ownership
    `assign()` takes ownership and `close()` closes the handle.
    `release()` hands it back.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. One wait may be pending at a time.

    @see win_stream_handle, timeout
*/
class BOOST_COROSIO_DECL win_object_handle : public io_object
{
public:
    /** Define backend hooks for kernel object waits.

        The IOCP backend derives from this to implement the wait.
    */
    struct implementation : io_object::implementation
    {
        /** Initiate an asynchronous wait for the object's signaled state.

            @param cont Continuation to resume on completion; owned by
                the awaitable.
            @param ex Executor for dispatching the completion.
            @param token Stop token for cancellation.
            @param ec Output error code.
            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> wait(
            capy::continuation& cont,
            capy::executor_ref ex,
            std::stop_token token,
            std::error_code* ec) = 0;

        /// Return the platform handle, or `INVALID_HANDLE_VALUE` when not open.
        virtual native_handle_type native_handle() const noexcept = 0;

        /** Release ownership of the native handle.

            Cancels a pending wait without closing the handle. The
            wait completes with a code that compares equal to
            `capy::cond::canceled`, unless the kernel already
            satisfied it, in which case it reports success. The
            caller takes ownership.

            @return The native handle.
        */
        virtual native_handle_type release_handle() noexcept = 0;

        /** Request cancellation of a pending wait.

            A wait the kernel has not yet satisfied completes with a
            code that compares equal to `capy::cond::canceled`.
        */
        virtual void cancel() noexcept = 0;
    };

    /// Represent the awaitable returned by @ref wait.
    struct wait_awaitable : detail::void_op_base<wait_awaitable>
    {
    private:
        friend win_object_handle;

        explicit wait_awaitable(win_object_handle& o) noexcept : o_(o) {}

        friend detail::void_op_base<wait_awaitable>;

        win_object_handle& o_;

        std::coroutine_handle<>
        dispatch(capy::continuation& cont, capy::executor_ref ex) const
        {
            return o_.get().wait(cont, ex, token_, &ec_);
        }
    };

    /** Closes the handle if open.

        A pending wait completes with a code that compares equal to
        `capy::cond::canceled`, unless the kernel already satisfied
        it, in which case it reports success.
    */
    ~win_object_handle() override;

    /** Construct from an execution context.

        @param ctx The execution context that owns this object.
    */
    explicit win_object_handle(capy::execution_context& ctx);

    /** Construct from an executor.

        The overload excludes `win_object_handle` itself so that it
        cannot displace the move constructor.

        @tparam Ex A type satisfying `capy::Executor`.
        @param ex The executor whose context owns this object.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, win_object_handle>) &&
        capy::Executor<Ex>
    explicit win_object_handle(Ex const& ex) : win_object_handle(ex.context())
    {
    }

    /** Transfer ownership of the handle from @p other.

        After the move, @p other is in a moved-from state and may only
        be destroyed or assigned to.

        @param other The object to move from.
        @pre No awaitables returned by @p other's methods exist.
    */
    win_object_handle(win_object_handle&& other) noexcept
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
    win_object_handle& operator=(win_object_handle&& other) noexcept
    {
        io_object::operator=(std::move(other));
        return *this;
    }

    /// Copy construction is disabled; the handle is uniquely owned.
    win_object_handle(win_object_handle const&) = delete;
    /// Copy assignment is disabled; the handle is uniquely owned.
    win_object_handle& operator=(win_object_handle const&) = delete;

    /** Adopt an existing waitable handle.

        No wait is performed on @p h; its signal state is unchanged.

        @param h The native handle to adopt.

        @return `error::already_open` if this object is open.
            `errc::bad_file_descriptor` when @p h is null, invalid,
            closed, or `GetCurrentProcess()` (which equals
            `INVALID_HANDLE_VALUE`). `errc::operation_not_supported`
            when @p h is any other pseudo-handle, lacks `SYNCHRONIZE`
            access, or is not a process, thread, event, semaphore,
            waitable timer, or job. The same when this `io_context`
            uses `locking_mode::unsafe`.
            Otherwise an empty code.

        @par Exception Safety
        Throws nothing. On failure the object is unchanged and @p h
        stays with the caller.

        @see release
    */
    [[nodiscard]] std::error_code assign(native_handle_type h) noexcept;

    /** Release ownership of the native handle.

        The object becomes not-open. A pending wait completes with a
        code that compares equal to `capy::cond::canceled`, unless the
        kernel already satisfied it, in which case it reports success.
        The caller is responsible for closing the result.

        @return The native handle.

        @throws std::system_error `errc::bad_file_descriptor` if the
            object is not open.

        @post `is_open() == false`
    */
    native_handle_type release();

    /** Close the handle.

        A pending wait completes with a code that compares equal to
        `capy::cond::canceled`, unless the kernel already satisfied
        it, in which case it reports success. Does nothing when not
        open.
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

    /** Cancel a pending wait.

        A wait the kernel has not yet satisfied completes with a code
        that compares equal to `capy::cond::canceled`.
    */
    void cancel() noexcept;

    /** Wait for the object to become signaled.

        Completes when the kernel satisfies the wait. Some objects
        change state when a wait is satisfied: an auto-reset event,
        a semaphore, or a waitable timer without manual reset. For
        these, the change has happened by the time this completes.
        A wait the kernel satisfied is reported as success even when
        it raced `cancel()`, `close()` or `release()`.

        Only one wait may be pending. While the first is pending, a
        second `wait()` completes with `errc::operation_in_progress`
        and does not disturb it. Calling `wait()` concurrently on a
        shared object is unsafe. There is no timeout parameter;
        compose with @ref timeout or a stop token.

        @return An awaitable yielding `capy::io_result<>`. Yields
            `errc::bad_file_descriptor` when not open and a code
            comparing equal to `capy::cond::canceled` when cancelled
            before the kernel satisfied the wait.

        @par Example
        @par !example wait

        @see timeout
    */
    [[nodiscard]] wait_awaitable wait()
    {
        return wait_awaitable(*this);
    }

protected:
    /// Default-construct (for derived types that initialize `io_object` directly).
    win_object_handle() noexcept = default;

    /** Construct from a handle.

        @param h The handle this object takes ownership of.
    */
    explicit win_object_handle(handle h) noexcept : io_object(std::move(h)) {}

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
