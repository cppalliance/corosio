//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
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
#include <boost/corosio/detail/op_base.hpp>

#ifndef BOOST_COROSIO_MRDOCS
#if BOOST_COROSIO_HAS_EPOLL || BOOST_COROSIO_HAS_SELECT || \
    BOOST_COROSIO_HAS_KQUEUE
#include <boost/corosio/native/detail/posix/posix_stream_file_service.hpp>
#endif

#if BOOST_COROSIO_HAS_URING
#include <boost/corosio/native/detail/uring/uring_stream_file.hpp>
#endif

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_file_service.hpp>
#endif
#endif // !BOOST_COROSIO_MRDOCS

namespace boost::corosio {

/** Reads and writes a file sequentially, calling the backend directly.

    This class template inherits from @ref stream_file. It shadows
    `read_some` / `write_some` with versions that call the backend
    implementation directly. The compiler can then inline through the
    entire call chain.

    Non-async operations (`open`, `close`, `size`, `resize`, `seek`,
    `sync_data`, `sync_all`) remain unchanged and dispatch through
    the compiled library.

    A `native_stream_file` IS-A `stream_file` and can be passed to
    any function expecting `stream_file&` or `io_stream&`, in which
    case virtual dispatch is used transparently.

    @note On POSIX platforms, file I/O is dispatched to a thread pool
    regardless of the chosen reactor backend. All three reactor tags
    (`epoll`, `select`, `kqueue`) therefore resolve to the same
    underlying implementation. The `Backend` template parameter
    exists for API symmetry with @ref native_tcp_socket and friends.
    The vtable savings are smaller relative to the thread-pool /
    overlapped-I/O cost than they are for socket operations.

    @tparam Backend A backend tag value (e.g., `epoll`, `iocp`).

    @par Thread Safety
    Same as @ref stream_file.

    @par Example
    @par !example native_stream_file

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
        : detail::bytes_op_base<native_read_awaitable<MutableBufferSequence>>
    {
        native_stream_file& self_;
        MutableBufferSequence buffers_;

        native_read_awaitable(
            native_stream_file& self, MutableBufferSequence buffers) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
        {
        }

        std::coroutine_handle<>
        dispatch(std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return self_.get_impl().read_some(
                h, ex, buffers_, this->token_, &this->ec_, &this->bytes_);
        }
    };

    template<class ConstBufferSequence>
    struct native_write_awaitable
        : detail::bytes_op_base<native_write_awaitable<ConstBufferSequence>>
    {
        native_stream_file& self_;
        ConstBufferSequence buffers_;

        native_write_awaitable(
            native_stream_file& self, ConstBufferSequence buffers) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
        {
        }

        std::coroutine_handle<>
        dispatch(std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return self_.get_impl().write_some(
                h, ex, buffers_, this->token_, &this->ec_, &this->bytes_);
        }
    };

public:
    /** Construct a native stream file from an execution context.

        @param ctx The execution context that owns this file.
    */
    explicit native_stream_file(capy::execution_context& ctx)
        : io_object(create_handle<service_type>(ctx))
    {
    }

    /** Construct a native stream file from an executor.

        @param ex The executor whose context owns this file.
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

    /// Copy construction is disabled; the handle is uniquely owned.
    native_stream_file(native_stream_file const&) = delete;
    /// Copy assignment is disabled; the handle is uniquely owned.
    native_stream_file& operator=(native_stream_file const&) = delete;

    /** Asynchronously read data from the file.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref io_stream::read_some.

        @param buffers The buffers to read into.

        @return An awaitable yielding the error code and the byte count read.
    */
    template<capy::MutableBufferSequence MB>
    [[nodiscard]] auto read_some(MB const& buffers)
    {
        return native_read_awaitable<MB>(*this, buffers);
    }

    /** Asynchronously write data to the file.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref io_stream::write_some.

        @param buffers The buffer data to write.

        @return An awaitable yielding the error code and the byte count written.
    */
    template<capy::ConstBufferSequence CB>
    [[nodiscard]] auto write_some(CB const& buffers)
    {
        return native_write_awaitable<CB>(*this, buffers);
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_NATIVE_NATIVE_STREAM_FILE_HPP
