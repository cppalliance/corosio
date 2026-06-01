//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_STREAM_FILE_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_STREAM_FILE_HPP

#include <boost/corosio/stream_file.hpp>
#include <boost/corosio/backend.hpp>

#ifndef BOOST_COROSIO_MRDOCS
#if BOOST_COROSIO_HAS_EPOLL || BOOST_COROSIO_HAS_SELECT || \
    BOOST_COROSIO_HAS_KQUEUE
#include <boost/corosio/native/detail/posix/posix_stream_file_service.hpp>
#endif

#if BOOST_COROSIO_HAS_IO_URING
#include <boost/corosio/native/detail/io_uring/io_uring_stream_file.hpp>
#endif

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_file_service.hpp>
#endif
#endif // !BOOST_COROSIO_MRDOCS

namespace boost::corosio {

/** A sequential file with devirtualized async I/O operations.

    This class template inherits from @ref stream_file and shadows
    `read_some` / `write_some` with versions that call the backend
    implementation directly, allowing the compiler to inline through
    the entire call chain.

    Non-async operations (`open`, `close`, `size`, `resize`, `seek`,
    `sync_data`, `sync_all`) remain unchanged and dispatch through
    the compiled library.

    A `native_stream_file` IS-A `stream_file` and can be passed to
    any function expecting `stream_file&` or `io_stream&`, in which
    case virtual dispatch is used transparently.

    @note On POSIX platforms, file I/O is dispatched to a thread
    pool regardless of the chosen reactor backend, so all three
    reactor tags (`epoll`, `select`, `kqueue`) resolve to the same
    underlying implementation. The `Backend` template parameter
    exists for API symmetry with @ref native_tcp_socket and friends.
    The vtable savings are smaller relative to the thread-pool /
    overlapped-I/O cost than they are for socket operations.

    @tparam Backend A backend tag value (e.g., `epoll`, `iocp`).

    @par Thread Safety
    Same as @ref stream_file.

    @par Example
    @code
    #include <boost/corosio/native/native_stream_file.hpp>

    native_io_context<epoll> ctx;
    native_stream_file<epoll> f(ctx);
    f.open("data.bin", file_base::read_only);
    char buf[4096];
    auto [ec, n] = co_await f.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));
    @endcode

    @see stream_file, epoll_t, iocp_t
*/
template<auto Backend>
class native_stream_file : public stream_file
{
    using backend_type = decltype(Backend);
    using impl_type    = typename backend_type::stream_file_type;
    using service_type = typename backend_type::stream_file_service_type;

    impl_type& get_impl() noexcept
    {
        return *static_cast<impl_type*>(h_.get());
    }

    template<class MutableBufferSequence>
    struct native_read_awaitable
    {
        native_stream_file& self_;
        MutableBufferSequence buffers_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        native_read_awaitable(
            native_stream_file& self,
            MutableBufferSequence buffers) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        capy::io_result<std::size_t> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(std::errc::operation_canceled), 0};
            return {ec_, bytes_transferred_};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            return self_.get_impl().read_some(
                h, env->executor, buffers_, token_, &ec_, &bytes_transferred_);
        }
    };

    template<class ConstBufferSequence>
    struct native_write_awaitable
    {
        native_stream_file& self_;
        ConstBufferSequence buffers_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        native_write_awaitable(
            native_stream_file& self,
            ConstBufferSequence buffers) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        capy::io_result<std::size_t> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(std::errc::operation_canceled), 0};
            return {ec_, bytes_transferred_};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            return self_.get_impl().write_some(
                h, env->executor, buffers_, token_, &ec_, &bytes_transferred_);
        }
    };

public:
    /** Construct a native stream file from an execution context.

        @param ctx The execution context that will own this file.
    */
    explicit native_stream_file(capy::execution_context& ctx)
        : io_object(create_handle<service_type>(ctx))
    {
    }

    /** Construct a native stream file from an executor.

        @param ex The executor whose context will own this file.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, native_stream_file>) &&
        capy::Executor<Ex>
    explicit native_stream_file(Ex const& ex) : native_stream_file(ex.context())
    {
    }

    /// Move construct.
    native_stream_file(native_stream_file&&) noexcept = default;

    /// Move assign.
    native_stream_file& operator=(native_stream_file&&) noexcept = default;

    native_stream_file(native_stream_file const&)            = delete;
    native_stream_file& operator=(native_stream_file const&) = delete;

    /** Asynchronously read data from the file.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref io_stream::read_some.
    */
    template<capy::MutableBufferSequence MB>
    auto read_some(MB const& buffers)
    {
        return native_read_awaitable<MB>(*this, buffers);
    }

    /** Asynchronously write data to the file.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref io_stream::write_some.
    */
    template<capy::ConstBufferSequence CB>
    auto write_some(CB const& buffers)
    {
        return native_write_awaitable<CB>(*this, buffers);
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_NATIVE_NATIVE_STREAM_FILE_HPP
