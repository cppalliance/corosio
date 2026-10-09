//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_WIN_STREAM_HANDLE_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_WIN_STREAM_HANDLE_HPP

#include <boost/corosio/detail/op_base.hpp>
#include <boost/corosio/win_stream_handle.hpp>
#include <boost/corosio/backend.hpp>

#if BOOST_COROSIO_HAS_IOCP || defined(BOOST_COROSIO_MRDOCS)

#ifndef BOOST_COROSIO_MRDOCS
#include <boost/corosio/native/detail/iocp/win_stream_handle_service.hpp>
#endif

namespace boost::corosio {

/** Drives an already-open overlapped Windows handle, calling the backend directly.

    This class template inherits from @ref win_stream_handle and
    shadows the async operations (`read_some`, `write_some`) with
    versions that call the backend implementation directly. This
    lets the compiler inline through the entire call chain.

    Non-async operations (`assign`, `release`, `close`, `cancel`)
    remain unchanged and dispatch through the compiled library.

    A `native_win_stream_handle` IS-A `win_stream_handle` and can be
    passed to any function expecting `win_stream_handle&` or
    `io_stream&`, in which case virtual dispatch is used
    transparently.

    @tparam Backend A backend tag value (e.g., `iocp`) whose type
        provides the concrete implementation types.

    @par Thread Safety
    Same as @ref win_stream_handle.

    @see win_stream_handle, iocp_t
*/
template<auto Backend>
class native_win_stream_handle : public win_stream_handle
{
    using backend_type = decltype(Backend);
    using impl_type    = typename backend_type::stream_handle_type;
    using service_type = typename backend_type::stream_handle_service_type;

    impl_type& get_impl() noexcept
    {
        return *static_cast<impl_type*>(h_.get());
    }

    template<class MutableBufferSequence>
    struct native_read_awaitable
        : detail::bytes_op_base<native_read_awaitable<MutableBufferSequence>>
    {
        native_win_stream_handle& self_;
        MutableBufferSequence buffers_;

        native_read_awaitable(
            native_win_stream_handle& self,
            MutableBufferSequence buffers) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
        {
        }

        std::coroutine_handle<>
        dispatch(capy::continuation& cont, capy::executor_ref ex) const
        {
            return self_.get_impl().read_some(
                cont, ex, buffers_, this->token_, &this->ec_, &this->bytes_);
        }
    };

    template<class ConstBufferSequence>
    struct native_write_awaitable
        : detail::bytes_op_base<native_write_awaitable<ConstBufferSequence>>
    {
        native_win_stream_handle& self_;
        ConstBufferSequence buffers_;

        native_write_awaitable(
            native_win_stream_handle& self,
            ConstBufferSequence buffers) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
        {
        }

        std::coroutine_handle<>
        dispatch(capy::continuation& cont, capy::executor_ref ex) const
        {
            return self_.get_impl().write_some(
                cont, ex, buffers_, this->token_, &this->ec_, &this->bytes_);
        }
    };

public:
    /** Construct a `native_win_stream_handle` from an execution context.

        @param ctx The execution context that owns this object.
    */
    explicit native_win_stream_handle(capy::execution_context& ctx)
        : io_object(handle(ctx, ctx.use_service<service_type>()))
    {
    }

    /** Construct a `native_win_stream_handle` from an executor.

        The overload excludes `native_win_stream_handle` itself so that
        it cannot displace the move constructor.

        @tparam Ex A type satisfying `capy::Executor`.
        @param ex The executor whose context owns this object.
    */
    template<class Ex>
        requires(!std::same_as<
                    std::remove_cvref_t<Ex>,
                    native_win_stream_handle>) &&
        capy::Executor<Ex>
    explicit native_win_stream_handle(Ex const& ex)
        : native_win_stream_handle(ex.context())
    {
    }

    /** Transfer ownership of the handle from the source.

        After the move, the source is in a moved-from state and may
        only be destroyed or assigned to.

        @pre No awaitables returned by the source's methods exist.
    */
    native_win_stream_handle(native_win_stream_handle&&) noexcept = default;

    /** Close any held handle and transfer ownership from the source.

        After the move, the source is in a moved-from state and may
        only be destroyed or assigned to.

        @return `*this`.
        @pre No awaitables returned by either object's methods exist.
    */
    native_win_stream_handle&
    operator=(native_win_stream_handle&&) noexcept = default;

    /// Copy construction is disabled; the handle is uniquely owned.
    native_win_stream_handle(native_win_stream_handle const&) = delete;
    /// Copy assignment is disabled; the handle is uniquely owned.
    native_win_stream_handle&
    operator=(native_win_stream_handle const&) = delete;

    /** Asynchronously read data from the handle.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref io_stream::read_some.

        @param buffers The buffer sequence to read into.

        @return An awaitable yielding `(error_code, std::size_t)`.
    */
    template<capy::MutableBufferSequence MB>
    [[nodiscard]] auto read_some(MB const& buffers)
    {
        return native_read_awaitable<MB>(*this, buffers);
    }

    /** Asynchronously write data to the handle.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref io_stream::write_some.

        @param buffers The buffer sequence to write from.

        @return An awaitable yielding `(error_code, std::size_t)`.
    */
    template<capy::ConstBufferSequence CB>
    [[nodiscard]] auto write_some(CB const& buffers)
    {
        return native_write_awaitable<CB>(*this, buffers);
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP || BOOST_COROSIO_MRDOCS

#endif // BOOST_COROSIO_NATIVE_NATIVE_WIN_STREAM_HANDLE_HPP
