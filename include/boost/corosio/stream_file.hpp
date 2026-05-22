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
#include <boost/corosio/file_base.hpp>
#include <boost/corosio/io/io_stream.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/concept/executor.hpp>

#include <concepts>
#include <cstdint>
#include <filesystem>

namespace boost::corosio {

/** An asynchronous sequential file for coroutine I/O.

    Provides asynchronous read and write operations on a regular
    file with an implicit position that advances after each
    operation.

    Inherits from @ref io_stream, so `read_some` and `write_some`
    are available and work with any algorithm that accepts an
    `io_stream&`.

    On POSIX platforms, file I/O is dispatched to a thread pool
    (blocking `preadv`/`pwritev`) with completion posted back to
    the scheduler. On Windows, true overlapped I/O is used via IOCP.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. Only one asynchronous operation
    may be in flight at a time.

    @par Example
    @code
    io_context ioc;
    stream_file f(ioc);
    f.open("data.bin", file_base::read_only);

    char buf[4096];
    auto [ec, n] = co_await f.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));
    if (ec == capy::cond::eof)
        // end of file
    @endcode
*/
class BOOST_COROSIO_DECL stream_file : public io_stream
{
public:
    /** Platform-specific file implementation interface.

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

        /// Return the file size in bytes.
        virtual std::uint64_t size() const = 0;

        /// Resize the file to @p new_size bytes.
        virtual void resize(std::uint64_t new_size) = 0;

        /// Synchronize file data to stable storage.
        virtual void sync_data() = 0;

        /// Synchronize file data and metadata to stable storage.
        virtual void sync_all() = 0;

        /// Release ownership of the native handle.
        virtual native_handle_type release() = 0;

        /// Adopt an existing native handle.
        virtual void assign(native_handle_type handle) = 0;

        /** Move the file position.

            @param offset Signed offset from @p origin.
            @param origin The reference point for the seek.
            @return The new absolute position.
        */
        virtual std::uint64_t
        seek(std::int64_t offset, file_base::seek_basis origin) = 0;
    };

    /** Destructor.

        Closes the file if open, cancelling any pending operations.
    */
    ~stream_file() override;

    /** Construct from an execution context.

        @param ctx The execution context that will own this file.
    */
    explicit stream_file(capy::execution_context& ctx);

    /** Construct from an executor.

        @param ex The executor whose context will own this file.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, stream_file>) &&
        capy::Executor<Ex>
    explicit stream_file(Ex const& ex) : stream_file(ex.context())
    {
    }

    /** Move constructor.

        Transfers ownership of the file resources.
    */
    stream_file(stream_file&& other) noexcept : io_object(std::move(other)) {}

    /** Move assignment operator.

        Closes any existing file and transfers ownership.
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

    stream_file(stream_file const&)            = delete;
    stream_file& operator=(stream_file const&) = delete;

    // read_some() inherited from io_read_stream
    // write_some() inherited from io_write_stream

    /** Open a file.

        @param path The filesystem path to open.
        @param mode Bitmask of @ref file_base::flags specifying
            access mode and creation behavior.

        @throws std::system_error on failure.
    */
    void open(
        std::filesystem::path const& path,
        file_base::flags mode = file_base::read_only);

    /** Close the file.

        Releases file resources. Any pending operations complete
        with `errc::operation_canceled`.
    */
    void close();

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

        All outstanding operations complete with
        `errc::operation_canceled`.
    */
    void cancel();

    /** Get the native file descriptor or handle.

        @return The native handle, or -1/INVALID_HANDLE_VALUE
            if not open.
    */
    native_handle_type native_handle() const noexcept;

    /** Return the file size in bytes.

        @throws std::system_error on failure.
    */
    std::uint64_t size() const;

    /** Resize the file to @p new_size bytes.

        @param new_size The new file size.
        @throws std::system_error on failure.
    */
    void resize(std::uint64_t new_size);

    /** Synchronize file data to stable storage.

        @throws std::system_error on failure.
    */
    void sync_data();

    /** Synchronize file data and metadata to stable storage.

        @throws std::system_error on failure.
    */
    void sync_all();

    /** Release ownership of the native handle.

        The file object becomes not-open. The caller is
        responsible for closing the returned handle.

        @return The native file descriptor or handle.
    */
    native_handle_type release();

    /** Adopt an existing native handle.

        Closes any currently open file before adopting.
        The file object takes ownership of the handle.

        @param handle The native file descriptor or handle.
        @throws std::system_error on failure.
    */
    void assign(native_handle_type handle);

    /** Move the file position.

        @param offset Signed offset from @p origin.
        @param origin The reference point for the seek.
        @return The new absolute position.
        @throws std::system_error on failure.
    */
    std::uint64_t
    seek(std::int64_t offset,
         file_base::seek_basis origin = file_base::seek_set);

protected:
    /// Default-construct (for derived types that initialize io_object directly).
    stream_file() noexcept = default;

    /// Construct from a pre-built handle (for native_stream_file).
    explicit stream_file(handle h) noexcept : io_object(std::move(h)) {}

private:
    inline implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_STREAM_FILE_HPP
