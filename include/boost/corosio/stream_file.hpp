//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_STREAM_FILE_HPP
#define BOOST_COROSIO_STREAM_FILE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/error.hpp>
#include <boost/corosio/file_base.hpp>
#include <boost/corosio/io/io_stream.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/concept/executor.hpp>
#include <boost/capy/io_result.hpp>

#include <concepts>
#include <cstdint>
#include <filesystem>
#include <system_error>

namespace boost::corosio {

/** Reads and writes a file sequentially, from a coroutine.

    Provides asynchronous read and write operations on a regular
    file with an implicit position that advances after each
    operation.

    Inherits from @ref io_stream, so `read_some` and `write_some`
    are available and work with any algorithm that accepts an
    `io_stream&`.

    On POSIX platforms, file I/O is dispatched to a thread pool
    (blocking `preadv`/`pwritev`) with completion posted back to
    the scheduler. On Windows, true overlapped I/O is used via IOCP.

    On Windows, while the file is open, its handle is bound to the
    execution context's completion port. Every overlapped call on the
    handle queues a packet to that port. Do not issue your own
    overlapped I/O on `native_handle()` (`DeviceIoControl`,
    `ReadFile`) unless the `OVERLAPPED`'s `hEvent` has its low-order
    bit set, which suppresses the packet.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. Only one asynchronous operation
    may be in flight at a time.

    @par Example
    @par !example stream_file
*/
class BOOST_COROSIO_DECL stream_file : public io_stream
{
public:
    /** Defines the file operations a platform backend implements.

        Backends derive from this to provide file I/O.
        `read_some` and `write_some` are inherited from
        @ref io_stream::implementation.
    */
    struct implementation : io_stream::implementation
    {
        /// Return the platform file descriptor or handle.
        virtual native_handle_type native_handle() const noexcept = 0;

        /// Cancel pending asynchronous operations.
        virtual void cancel() noexcept = 0;

        /** Return the file size in bytes.

            @return The current size of the file, in bytes.

            @throws std::system_error if the underlying size query fails.
        */
        virtual std::uint64_t size() const = 0;

        /** Resize the file to @p new_size bytes.

            @param new_size The requested size in bytes.

            @return The error code, empty on success.
        */
        virtual std::error_code resize(std::uint64_t new_size) noexcept = 0;

        /** Synchronize file data to stable storage.

            @return The error code, empty on success.
        */
        virtual std::error_code sync_data() noexcept = 0;

        /** Synchronize file data and metadata to stable storage.

            @return The error code, empty on success.
        */
        virtual std::error_code sync_all() noexcept = 0;

        /** Release ownership of the native handle.

            @return The native handle, which the caller now owns.

            @throws std::system_error if the file is not open.
        */
        virtual native_handle_type release() = 0;

        /** Adopt an existing native handle.

            @param handle The native handle to adopt. The implementation takes
                ownership and closes it.

            @return The error code, empty on success.
        */
        virtual std::error_code assign(native_handle_type handle) noexcept = 0;

        /** Move the file position.

            @param offset Signed offset from @p origin.
            @param origin The reference point for the seek.
            @return The error code and new absolute position.
        */
        virtual capy::io_result<std::uint64_t>
        seek(std::int64_t offset, file_base::seek_basis origin) noexcept = 0;
    };

    /** Closes the file if open, cancelling any pending operations.
    */
    ~stream_file() override;

    /** Construct from an execution context.

        @param ctx The execution context that owns this file.
    */
    explicit stream_file(capy::execution_context& ctx);

    /** Construct from an executor.

        @param ex The executor whose context owns this file.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, stream_file>) &&
        capy::Executor<Ex>
    explicit stream_file(Ex const& ex) : stream_file(ex.context())
    {
    }

    /** Transfers ownership of the file resources.
    */
    stream_file(stream_file&& other) noexcept : io_object(std::move(other)) {}

    /** Closes any existing file and transfers ownership.

        @return Reference to this object.
    */
    stream_file& operator=(stream_file&& other) noexcept
    {
        if (this != &other)
        {
            close();
            h_ = std::move(other.h_);
        }
        return *this;
    }

    /// Copy construction is disabled; the handle is uniquely owned.
    stream_file(stream_file const&) = delete;
    /// Copy assignment is disabled; the handle is uniquely owned.
    stream_file& operator=(stream_file const&) = delete;

    // read_some() inherited from io_read_stream
    // write_some() inherited from io_write_stream

    /** Open a file.

        Failures such as a missing file or insufficient permissions
        are expected runtime conditions and are reported through the
        returned error code. If the file is already open, it is
        closed first.

        @param path The filesystem path to open.
        @param mode Bitmask of @ref file_base::flags specifying
            access mode and creation behavior.

        @return The error code, empty on success.
    */
    [[nodiscard]] std::error_code open(
        std::filesystem::path const& path,
        file_base::flags mode = file_base::read_only) noexcept;

    /** Close the file.

        Releases file resources. Pending operations complete through the
        same path as @ref cancel: one still in flight completes with
        `errc::operation_canceled`. An operation whose result is already
        decided reports that result.
    */
    void close() noexcept;

    /** Check if the file is open.

        @return `true` if the file is open and ready for I/O.
    */
    bool is_open() const noexcept
    {
#if BOOST_COROSIO_HAS_IOCP && !defined(BOOST_COROSIO_MRDOCS)
        return h_ && get().native_handle() != ~native_handle_type(0);
#else
        return h_ && get().native_handle() >= 0;
#endif
    }

    /** Cancel pending asynchronous operations.

        Operations still in flight complete with
        `errc::operation_canceled`; an operation whose result is
        already decided reports that result.
    */
    void cancel() noexcept;

    /** Get the native file descriptor or handle.

        @return The native handle, or -1/INVALID_HANDLE_VALUE
            if not open.
    */
    native_handle_type native_handle() const noexcept;

    /** Return the file size in bytes.

        @return The file size in bytes.

        @throws std::system_error If the file is not open, or if the
            underlying size query fails.
    */
    std::uint64_t size() const;

    /** Resize the file to @p new_size bytes.

        Failures such as insufficient disk space are reported
        through the returned error code. A closed file reports
        `errc::bad_file_descriptor`.

        @param new_size The new file size.

        @return The error code, empty on success.
    */
    [[nodiscard]] std::error_code resize(std::uint64_t new_size) noexcept;

    /** Synchronize file data to stable storage.

        Write-back failures such as device I/O errors surface here
        and are reported through the returned error code. A closed
        file reports `errc::bad_file_descriptor`.

        @return The error code, empty on success.
    */
    [[nodiscard]] std::error_code sync_data() noexcept;

    /** Synchronize file data and metadata to stable storage.

        Write-back failures such as device I/O errors surface here
        and are reported through the returned error code. A closed
        file reports `errc::bad_file_descriptor`.

        @return The error code, empty on success.
    */
    [[nodiscard]] std::error_code sync_all() noexcept;

    /** Release ownership of the native handle.

        The file object becomes not-open. The caller is
        responsible for closing the returned handle.

        `release()` cancels pending operations first. On Windows, the
        object keeps the handle and this throws if one is still in
        flight. It does the same if Windows refuses to detach the
        handle from the execution context's completion port. Call
        `release()` again once the cancelled operations have
        completed. Detaching requires Windows 8.1 or later.

        @return The native file descriptor or handle.

        @throws std::system_error `errc::bad_file_descriptor` if the
            file is not open. On Windows,
            `errc::device_or_resource_busy` if an operation is still in
            flight, or `errc::operation_not_supported` if the handle
            cannot be detached.
    */
    native_handle_type release();

    /** Adopt an existing native handle.

        The object must be closed. To replace a held file, `close()`
        or `release()` it first. On success the object takes
        ownership of @p handle. Handles created elsewhere may be
        unsuitable for asynchronous I/O. `assign()` reports most such
        failures through the returned error code.

        @param handle The native file descriptor or handle.

        @return An error code describing the outcome.
            `error::already_open` if this object is open.
            `errc::bad_file_descriptor` if @p handle is invalid.
            `errc::operation_not_supported` if a file object cannot
            use it. On Windows, the rejected handles are a pipe, a
            socket, a console, a directory, a handle opened without
            `FILE_FLAG_OVERLAPPED`, or one
            already in skip-completion-port-on-success mode. On
            Windows, `errc::invalid_argument` when @p handle is
            bound to another completion port. Any other failure is
            the code reported by the system. Otherwise, the code is
            empty.

        @par Exception Safety
        Throws nothing. On failure the object is unchanged and the
        caller still owns @p handle.

        @note On POSIX, the rejected descriptors are, in practice, a
            directory, a pipe, a socket, or any other anonymous inode.
            Adopt a pipe, a socket, or an anonymous inode into a
            @ref posix_stream_descriptor instead. `assign()` accepts a
            non-seekable character device such as a tty. Its first
            read or write then fails with `ESPIPE` on the epoll,
            kqueue, and select I/O backends.

        @see release
    */
    [[nodiscard]] std::error_code assign(native_handle_type handle) noexcept;

    /** Move the file position.

        Positions beyond the end of the file are allowed. A
        resulting negative position is reported through the error
        code, as offsets often originate from file contents. A
        closed file reports `errc::bad_file_descriptor`.

        @param offset Signed offset from @p origin.
        @param origin The reference point for the seek.

        @return The error code and new absolute position.
    */
    [[nodiscard]] capy::io_result<std::uint64_t> seek(
        std::int64_t offset,
        file_base::seek_basis origin = file_base::seek_set) noexcept;

protected:
    /// Default-construct (for derived types that initialize `io_object` directly).
    stream_file() noexcept = default;

    /** Construct from a pre-built handle (for `native_stream_file`).

        @param h The pre-built handle to adopt.
    */
    explicit stream_file(handle h) noexcept : io_object(std::move(h)) {}

private:
    inline implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_STREAM_FILE_HPP
