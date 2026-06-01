//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_RANDOM_ACCESS_FILE_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_RANDOM_ACCESS_FILE_HPP

#include <boost/corosio/random_access_file.hpp>
#include <boost/corosio/backend.hpp>

#ifndef BOOST_COROSIO_MRDOCS
#if BOOST_COROSIO_HAS_EPOLL || BOOST_COROSIO_HAS_SELECT || \
    BOOST_COROSIO_HAS_KQUEUE
#include <boost/corosio/native/detail/posix/posix_random_access_file_service.hpp>
#endif

#if BOOST_COROSIO_HAS_IO_URING
#include <boost/corosio/native/detail/io_uring/io_uring_random_access_file.hpp>
#endif

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_random_access_file_service.hpp>
#endif
#endif // !BOOST_COROSIO_MRDOCS

namespace boost::corosio {

/** A random-access file with devirtualized async I/O operations.

    This class template inherits from @ref random_access_file and
    shadows `read_some_at` / `write_some_at` with versions that
    call the backend implementation directly, allowing the compiler
    to inline through the entire call chain.

    Non-async operations (`open`, `close`, `size`, `resize`,
    `sync_data`, `sync_all`) remain unchanged and dispatch through
    the compiled library.

    A `native_random_access_file` IS-A `random_access_file` and
    can be passed to any function expecting `random_access_file&`,
    in which case virtual dispatch is used transparently.

    @note On POSIX platforms, file I/O is dispatched to a thread
    pool regardless of the chosen reactor backend, so all three
    reactor tags (`epoll`, `select`, `kqueue`) resolve to the same
    underlying implementation. The `Backend` template parameter
    exists for API symmetry with @ref native_tcp_socket and friends.
    The vtable savings are smaller relative to the thread-pool /
    overlapped-I/O cost than they are for socket operations.

    @tparam Backend A backend tag value (e.g., `epoll`, `iocp`).

    @par Thread Safety
    Same as @ref random_access_file.

    @par Example
    @code
    #include <boost/corosio/native/native_random_access_file.hpp>

    native_io_context<epoll> ctx;
    native_random_access_file<epoll> f(ctx);
    f.open("data.bin", file_base::read_only);
    char buf[4096];
    auto [ec, n] = co_await f.read_some_at(
        0, capy::mutable_buffer(buf, sizeof(buf)));
    @endcode

    @see random_access_file, epoll_t, iocp_t
*/
template<auto Backend>
class native_random_access_file : public random_access_file
{
    using backend_type = decltype(Backend);
    using impl_type    = typename backend_type::random_access_file_type;
    using service_type =
        typename backend_type::random_access_file_service_type;

    impl_type& get_impl() noexcept
    {
        return *static_cast<impl_type*>(h_.get());
    }

    template<class MutableBufferSequence>
    struct native_read_at_awaitable
    {
        native_random_access_file& self_;
        std::uint64_t offset_;
        MutableBufferSequence buffers_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        native_read_at_awaitable(
            native_random_access_file& self,
            std::uint64_t offset,
            MutableBufferSequence buffers) noexcept
            : self_(self)
            , offset_(offset)
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
            return self_.get_impl().read_some_at(
                offset_, h, env->executor, buffers_,
                token_, &ec_, &bytes_transferred_);
        }
    };

    template<class ConstBufferSequence>
    struct native_write_at_awaitable
    {
        native_random_access_file& self_;
        std::uint64_t offset_;
        ConstBufferSequence buffers_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        native_write_at_awaitable(
            native_random_access_file& self,
            std::uint64_t offset,
            ConstBufferSequence buffers) noexcept
            : self_(self)
            , offset_(offset)
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
            return self_.get_impl().write_some_at(
                offset_, h, env->executor, buffers_,
                token_, &ec_, &bytes_transferred_);
        }
    };

public:
    /** Construct a native random-access file from an execution context.

        @param ctx The execution context that will own this file.
    */
    explicit native_random_access_file(capy::execution_context& ctx)
        : random_access_file(create_handle<service_type>(ctx))
    {
    }

    /** Construct a native random-access file from an executor.

        @param ex The executor whose context will own this file.
    */
    template<class Ex>
        requires(!std::same_as<
                 std::remove_cvref_t<Ex>,
                 native_random_access_file>) &&
        capy::Executor<Ex>
    explicit native_random_access_file(Ex const& ex)
        : native_random_access_file(ex.context())
    {
    }

    /// Move construct.
    native_random_access_file(native_random_access_file&&) noexcept = default;

    /// Move assign.
    native_random_access_file&
    operator=(native_random_access_file&&) noexcept = default;

    native_random_access_file(native_random_access_file const&) = delete;
    native_random_access_file&
    operator=(native_random_access_file const&) = delete;

    /** Asynchronously read at the given offset.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref random_access_file::read_some_at.
    */
    template<capy::MutableBufferSequence MB>
    auto read_some_at(std::uint64_t offset, MB const& buffers)
    {
        return native_read_at_awaitable<MB>(*this, offset, buffers);
    }

    /** Asynchronously write at the given offset.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref random_access_file::write_some_at.
    */
    template<capy::ConstBufferSequence CB>
    auto write_some_at(std::uint64_t offset, CB const& buffers)
    {
        return native_write_at_awaitable<CB>(*this, offset, buffers);
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_NATIVE_NATIVE_RANDOM_ACCESS_FILE_HPP
