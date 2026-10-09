//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_WIN_RANDOM_ACCESS_HANDLE_HPP
#define BOOST_COROSIO_WIN_RANDOM_ACCESS_HANDLE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP || defined(BOOST_COROSIO_MRDOCS)

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/detail/buffer_param.hpp>
#include <boost/corosio/detail/op_base.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/concept/executor.hpp>
#include <boost/capy/buffers.hpp>

#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <system_error>
#include <type_traits>

namespace boost::corosio {

/** Drives an already-open overlapped Windows handle with caller-chosen offsets.

    Wraps an overlapped handle whose I/O is positional. Examples
    are a volume such as `C:` or a physical drive such as
    `PhysicalDrive0`, opened by device path with
    `FILE_FLAG_OVERLAPPED`. Any other overlapped handle works too,
    unless it is a console, a directory, a socket, a
    synchronous-mode handle, or one already in
    skip-completion-port-on-success mode. The handle must come
    from the caller; this type never creates one.
    For regular files prefer @ref random_access_file, which also
    offers `size()`, `resize()` and the sync operations.

    Overlapped pipes are accepted too. The kernel ignores the offset
    for them, so each operation reads or writes the stream in order
    of arrival.

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

    @par Concurrency
    Any number of `read_some_at()` and `write_some_at()` operations
    may be in flight at once.

    @par Transfers
    Each operation transfers at most the first non-empty buffer of
    the sequence. Volume and drive handles require sector-aligned
    offsets, lengths and buffers; misaligned requests fail with the
    kernel's error.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe, except that concurrent positional
    operations are supported.

    @par Example
    @par !example win_random_access_handle

    @see random_access_file, win_stream_handle
*/
class BOOST_COROSIO_DECL win_random_access_handle : public io_object
{
public:
    /** Declares the offset-based operations the IOCP backend must implement. */
    struct implementation : io_object::implementation
    {
        /** Initiate a read at the given offset.

            @param offset Byte offset into the handle.
            @param cont The awaiting coroutine's continuation. It must
                stay valid until `cont.h` is resumed through @p ex.
            @param ex Executor for dispatching the completion.
            @param buf The buffer to read into.
            @param token Stop token for cancellation.
            @param ec Output error code.
            @param bytes_out Output bytes transferred.
            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> read_some_at(
            std::uint64_t offset,
            capy::continuation& cont,
            capy::executor_ref ex,
            buffer_param buf,
            std::stop_token token,
            std::error_code* ec,
            std::size_t* bytes_out) = 0;

        /** Initiate a write at the given offset.

            @param offset Byte offset into the handle.
            @param cont The awaiting coroutine's continuation. It must
                stay valid until `cont.h` is resumed through @p ex.
            @param ex Executor for dispatching the completion.
            @param buf The buffer to write from.
            @param token Stop token for cancellation.
            @param ec Output error code.
            @param bytes_out Output bytes transferred.
            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> write_some_at(
            std::uint64_t offset,
            capy::continuation& cont,
            capy::executor_ref ex,
            buffer_param buf,
            std::stop_token token,
            std::error_code* ec,
            std::size_t* bytes_out) = 0;

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

    /** Yields `(error_code, std::size_t)` once a positional read completes. */
    template<class MutableBufferSequence>
    struct read_some_at_awaitable
        : detail::bytes_op_base<read_some_at_awaitable<MutableBufferSequence>>
    {
    private:
        friend win_random_access_handle;
        friend detail::bytes_op_base<
            read_some_at_awaitable<MutableBufferSequence>>;

        win_random_access_handle& f_;
        std::uint64_t offset_;
        MutableBufferSequence buffers_;

        read_some_at_awaitable(
            win_random_access_handle& f,
            std::uint64_t offset,
            MutableBufferSequence
                buffers) noexcept(std::
                                      is_nothrow_move_constructible_v<
                                          MutableBufferSequence>)
            : f_(f)
            , offset_(offset)
            , buffers_(std::move(buffers))
        {
        }

        std::coroutine_handle<>
        dispatch(capy::continuation& cont, capy::executor_ref ex) const
        {
            return f_.get().read_some_at(
                offset_, cont, ex, buffers_, this->token_, &this->ec_,
                &this->bytes_);
        }
    };

    /** Yields `(error_code, std::size_t)` once a positional write completes. */
    template<class ConstBufferSequence>
    struct write_some_at_awaitable
        : detail::bytes_op_base<write_some_at_awaitable<ConstBufferSequence>>
    {
    private:
        friend win_random_access_handle;
        friend detail::bytes_op_base<
            write_some_at_awaitable<ConstBufferSequence>>;

        win_random_access_handle& f_;
        std::uint64_t offset_;
        ConstBufferSequence buffers_;

        write_some_at_awaitable(
            win_random_access_handle& f,
            std::uint64_t offset,
            ConstBufferSequence
                buffers) noexcept(std::
                                      is_nothrow_move_constructible_v<
                                          ConstBufferSequence>)
            : f_(f)
            , offset_(offset)
            , buffers_(std::move(buffers))
        {
        }

        std::coroutine_handle<>
        dispatch(capy::continuation& cont, capy::executor_ref ex) const
        {
            return f_.get().write_some_at(
                offset_, cont, ex, buffers_, this->token_, &this->ec_,
                &this->bytes_);
        }
    };

    /** Closes the handle if open, cancelling pending operations. */
    ~win_random_access_handle() override;

    /** Construct from an execution context.

        @param ctx The execution context that owns this object.
    */
    explicit win_random_access_handle(capy::execution_context& ctx);

    /** Construct from an executor.

        The overload excludes `win_random_access_handle` itself so
        that it cannot displace the move constructor.

        @tparam Ex A type satisfying `capy::Executor`.
        @param ex The executor whose context owns this object.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, win_random_access_handle>) &&
        capy::Executor<Ex>
    explicit win_random_access_handle(Ex const& ex)
        : win_random_access_handle(ex.context())
    {
    }

    /** Transfer ownership of the handle from @p other.

        After the move, @p other is in a moved-from state and may only
        be destroyed or assigned to.

        @param other The object to move from.
        @pre No awaitables returned by @p other's methods exist.
    */
    win_random_access_handle(win_random_access_handle&& other) noexcept
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
    win_random_access_handle& operator=(win_random_access_handle&& other) noexcept
    {
        io_object::operator=(std::move(other));
        return *this;
    }

    /// Copy construction is disabled; the handle is uniquely owned.
    win_random_access_handle(win_random_access_handle const&) = delete;
    /// Copy assignment is disabled; the handle is uniquely owned.
    win_random_access_handle& operator=(win_random_access_handle const&) = delete;

    /** Adopt an existing overlapped handle.

        @param h The native handle to adopt.

        @return `error::already_open` if this object is open.
            `errc::invalid_argument` when @p h is bound to another
            completion port. `errc::bad_file_descriptor` when @p h is
            null, invalid, or closed. `errc::operation_not_supported`
            when @p h is a console, a socket, a directory, a
            synchronous-mode handle, or one already in
            skip-completion-port-on-success mode. It is also returned
            when the handle's I/O mode cannot be queried. Otherwise
            the error the system reported, or an empty code.

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

    /** Read data at the given offset.

        @param offset Byte offset into the handle.
        @param buffers The buffer sequence to read into.

        @return An awaitable yielding `(error_code, std::size_t)`.

        A closed handle reports `errc::bad_file_descriptor`.
    */
    template<capy::MutableBufferSequence MB>
    [[nodiscard]] auto read_some_at(std::uint64_t offset, MB const& buffers)
    {
        read_some_at_awaitable<MB> aw(*this, offset, buffers);
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /** Write data at the given offset.

        @param offset Byte offset into the handle.
        @param buffers The buffer sequence to write from.

        @return An awaitable yielding `(error_code, std::size_t)`.

        A closed handle reports `errc::bad_file_descriptor`.
    */
    template<capy::ConstBufferSequence CB>
    [[nodiscard]] auto write_some_at(std::uint64_t offset, CB const& buffers)
    {
        write_some_at_awaitable<CB> aw(*this, offset, buffers);
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
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
    win_random_access_handle() noexcept = default;

    /** Construct from a handle.

        @param h The handle this object takes ownership of.
    */
    explicit win_random_access_handle(handle h) noexcept
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

#endif // BOOST_COROSIO_HAS_IOCP || BOOST_COROSIO_MRDOCS

#endif
