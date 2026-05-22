//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_RANDOM_ACCESS_FILE_HPP
#define BOOST_COROSIO_RANDOM_ACCESS_FILE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/detail/buffer_param.hpp>
#include <boost/corosio/file_base.hpp>
#include <boost/corosio/io/io_object.hpp>
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
#include <type_traits>
#include <filesystem>
#include <stop_token>
#include <system_error>

namespace boost::corosio {

/** An asynchronous random-access file for coroutine I/O.

    Provides asynchronous read and write operations at explicit
    byte offsets, without maintaining an implicit file position.

    On POSIX platforms, file I/O is dispatched to a thread pool
    (blocking `preadv`/`pwritev`) with completion posted back to
    the scheduler. On Windows, true overlapped I/O is used via IOCP.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. Multiple concurrent reads and writes
    are supported from coroutines sharing the same file object,
    but external synchronization is required for non-async
    operations (open, close, size, resize, etc.).

    @par Example
    @code
    io_context ioc;
    random_access_file f(ioc);
    f.open("data.bin", file_base::read_only);

    char buf[4096];
    auto [ec, n] = co_await f.read_some_at(
        0, capy::mutable_buffer(buf, sizeof(buf)));
    @endcode
*/
class BOOST_COROSIO_DECL random_access_file : public io_object
{
public:
    /** Platform-specific random-access file implementation interface.

        Backends derive from this to provide offset-based file I/O.
    */
    struct implementation : io_object::implementation
    {
        /** Initiate a read at the given offset.

            @param offset Byte offset into the file.
            @param h Coroutine handle to resume on completion.
            @param ex Executor for dispatching the completion.
            @param buf The buffer to read into.
            @param token Stop token for cancellation.
            @param ec Output error code.
            @param bytes_out Output bytes transferred.
            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> read_some_at(
            std::uint64_t offset,
            std::coroutine_handle<> h,
            capy::executor_ref ex,
            buffer_param buf,
            std::stop_token token,
            std::error_code* ec,
            std::size_t* bytes_out) = 0;

        /** Initiate a write at the given offset.

            @param offset Byte offset into the file.
            @param h Coroutine handle to resume on completion.
            @param ex Executor for dispatching the completion.
            @param buf The buffer to write from.
            @param token Stop token for cancellation.
            @param ec Output error code.
            @param bytes_out Output bytes transferred.
            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> write_some_at(
            std::uint64_t offset,
            std::coroutine_handle<> h,
            capy::executor_ref ex,
            buffer_param buf,
            std::stop_token token,
            std::error_code* ec,
            std::size_t* bytes_out) = 0;

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
    };

    /** Awaitable for async read-at operations. */
    template<class MutableBufferSequence>
    struct read_some_at_awaitable
    {
        random_access_file& f_;
        std::uint64_t offset_;
        MutableBufferSequence buffers_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_ = 0;

        read_some_at_awaitable(
            random_access_file& f,
            std::uint64_t offset,
            MutableBufferSequence buffers)
            noexcept(std::is_nothrow_move_constructible_v<MutableBufferSequence>)
            : f_(f)
            , offset_(offset)
            , buffers_(std::move(buffers))
        {
        }

        bool await_ready() const noexcept
        {
            return false;
        }

        capy::io_result<std::size_t> await_resume() const noexcept
        {
            return {ec_, bytes_};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            return f_.get().read_some_at(
                offset_, h, env->executor, buffers_, token_, &ec_, &bytes_);
        }
    };

    /** Awaitable for async write-at operations. */
    template<class ConstBufferSequence>
    struct write_some_at_awaitable
    {
        random_access_file& f_;
        std::uint64_t offset_;
        ConstBufferSequence buffers_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_ = 0;

        write_some_at_awaitable(
            random_access_file& f,
            std::uint64_t offset,
            ConstBufferSequence buffers)
            noexcept(std::is_nothrow_move_constructible_v<ConstBufferSequence>)
            : f_(f)
            , offset_(offset)
            , buffers_(std::move(buffers))
        {
        }

        bool await_ready() const noexcept
        {
            return false;
        }

        capy::io_result<std::size_t> await_resume() const noexcept
        {
            return {ec_, bytes_};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            return f_.get().write_some_at(
                offset_, h, env->executor, buffers_, token_, &ec_, &bytes_);
        }
    };

public:
    /** Destructor.

        Closes the file if open, cancelling any pending operations.
    */
    ~random_access_file() override;

    /** Construct from an execution context.

        @param ctx The execution context that will own this file.
    */
    explicit random_access_file(capy::execution_context& ctx);

    /** Construct from an executor.

        @param ex The executor whose context will own this file.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, random_access_file>) &&
        capy::Executor<Ex>
    explicit random_access_file(Ex const& ex) : random_access_file(ex.context())
    {
    }

    /** Move constructor. */
    random_access_file(random_access_file&& other) noexcept
        : io_object(std::move(other))
    {
    }

    /** Move assignment operator. */
    random_access_file& operator=(random_access_file&& other) noexcept
    {
        if (this != &other)
        {
            close();
            h_ = std::move(other.h_);
        }
        return *this;
    }

    random_access_file(random_access_file const&)            = delete;
    random_access_file& operator=(random_access_file const&) = delete;

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

    /** Check if the file is open. */
    bool is_open() const noexcept
    {
#if BOOST_COROSIO_HAS_IOCP && !defined(BOOST_COROSIO_MRDOCS)
        return h_ && get().native_handle() != ~native_handle_type(0);
#else
        return h_ && get().native_handle() >= 0;
#endif
    }

    /** Read data at the given offset.

        @param offset Byte offset into the file.
        @param buffers The buffer sequence to read into.

        @return An awaitable yielding `(error_code, std::size_t)`.

        @throws std::logic_error if the file is not open.
    */
    template<capy::MutableBufferSequence MB>
    auto read_some_at(std::uint64_t offset, MB const& buffers)
    {
        if (!is_open())
            detail::throw_logic_error("read_some_at: file not open");
        return read_some_at_awaitable<MB>(*this, offset, buffers);
    }

    /** Write data at the given offset.

        @param offset Byte offset into the file.
        @param buffers The buffer sequence to write from.

        @return An awaitable yielding `(error_code, std::size_t)`.

        @throws std::logic_error if the file is not open.
    */
    template<capy::ConstBufferSequence CB>
    auto write_some_at(std::uint64_t offset, CB const& buffers)
    {
        if (!is_open())
            detail::throw_logic_error("write_some_at: file not open");
        return write_some_at_awaitable<CB>(*this, offset, buffers);
    }

    /** Cancel pending asynchronous operations. */
    void cancel();

    /** Get the native file descriptor or handle. */
    native_handle_type native_handle() const noexcept;

    /** Return the file size in bytes. */
    std::uint64_t size() const;

    /** Resize the file. */
    void resize(std::uint64_t new_size);

    /** Synchronize file data to stable storage. */
    void sync_data();

    /** Synchronize file data and metadata to stable storage. */
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
    */
    void assign(native_handle_type handle);

protected:
    /// Construct from a pre-built handle (for native_random_access_file).
    explicit random_access_file(handle h) noexcept : io_object(std::move(h)) {}

private:
    inline implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_RANDOM_ACCESS_FILE_HPP
