//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_WIN_RANDOM_ACCESS_HANDLE_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_WIN_RANDOM_ACCESS_HANDLE_HPP

#include <boost/corosio/detail/op_base.hpp>
#include <boost/corosio/win_random_access_handle.hpp>
#include <boost/corosio/backend.hpp>

#if BOOST_COROSIO_HAS_IOCP || defined(BOOST_COROSIO_MRDOCS)

#ifndef BOOST_COROSIO_MRDOCS
#include <boost/corosio/native/detail/iocp/win_random_access_handle_service.hpp>
#endif

namespace boost::corosio {

/** Drives an already-open overlapped handle at arbitrary offsets, calling the backend directly.

    This class template inherits from @ref win_random_access_handle
    and shadows `read_some_at` / `write_some_at` with versions that
    call the backend implementation directly. The compiler can then
    inline through the entire call chain.

    Non-async operations (`assign`, `release`, `close`, `cancel`)
    remain unchanged and dispatch through the compiled library.

    A `native_win_random_access_handle` IS-A
    `win_random_access_handle` and can be passed to any function
    expecting `win_random_access_handle&`, in which case virtual
    dispatch is used transparently.

    @tparam Backend A backend tag value (e.g., `iocp`) whose type
        provides the concrete implementation types.

    @par Thread Safety
    Same as @ref win_random_access_handle.

    @par Example
    @par !example native_win_random_access_handle

    @see win_random_access_handle, iocp_t
*/
template<auto Backend>
class native_win_random_access_handle : public win_random_access_handle
{
    using backend_type = decltype(Backend);
    using impl_type    = typename backend_type::random_access_handle_type;
    using service_type =
        typename backend_type::random_access_handle_service_type;

    impl_type& get_impl() noexcept
    {
        return *static_cast<impl_type*>(h_.get());
    }

    template<class MutableBufferSequence>
    struct native_read_at_awaitable
        : detail::bytes_op_base<native_read_at_awaitable<MutableBufferSequence>>
    {
        native_win_random_access_handle& self_;
        std::uint64_t offset_;
        MutableBufferSequence buffers_;

        native_read_at_awaitable(
            native_win_random_access_handle& self,
            std::uint64_t offset,
            MutableBufferSequence buffers) noexcept
            : self_(self)
            , offset_(offset)
            , buffers_(std::move(buffers))
        {
        }

        std::coroutine_handle<>
        dispatch(capy::continuation& cont, capy::executor_ref ex) const
        {
            return self_.get_impl().read_some_at(
                offset_, cont, ex, buffers_, this->token_, &this->ec_,
                &this->bytes_);
        }
    };

    template<class ConstBufferSequence>
    struct native_write_at_awaitable
        : detail::bytes_op_base<native_write_at_awaitable<ConstBufferSequence>>
    {
        native_win_random_access_handle& self_;
        std::uint64_t offset_;
        ConstBufferSequence buffers_;

        native_write_at_awaitable(
            native_win_random_access_handle& self,
            std::uint64_t offset,
            ConstBufferSequence buffers) noexcept
            : self_(self)
            , offset_(offset)
            , buffers_(std::move(buffers))
        {
        }

        std::coroutine_handle<>
        dispatch(capy::continuation& cont, capy::executor_ref ex) const
        {
            return self_.get_impl().write_some_at(
                offset_, cont, ex, buffers_, this->token_, &this->ec_,
                &this->bytes_);
        }
    };

public:
    /** Construct a `native_win_random_access_handle` from an execution context.

        @param ctx The execution context that owns this object.
    */
    explicit native_win_random_access_handle(capy::execution_context& ctx)
        : win_random_access_handle(handle(ctx, ctx.use_service<service_type>()))
    {
    }

    /** Construct a `native_win_random_access_handle` from an executor.

        The overload excludes `native_win_random_access_handle` itself
        so that it cannot displace the move constructor.

        @tparam Ex A type satisfying `capy::Executor`.
        @param ex The executor whose context owns this object.
    */
    template<class Ex>
        requires(!std::same_as<
                    std::remove_cvref_t<Ex>,
                    native_win_random_access_handle>) &&
        capy::Executor<Ex>
    explicit native_win_random_access_handle(Ex const& ex)
        : native_win_random_access_handle(ex.context())
    {
    }

    /** Transfer ownership of the handle from the source.

        After the move, the source is in a moved-from state and may
        only be destroyed or assigned to.

        @pre No awaitables returned by the source's methods exist.
    */
    native_win_random_access_handle(
        native_win_random_access_handle&&) noexcept = default;

    /** Close any held handle and transfer ownership from the source.

        After the move, the source is in a moved-from state and may
        only be destroyed or assigned to.

        @return `*this`.
        @pre No awaitables returned by either object's methods exist.
    */
    native_win_random_access_handle&
    operator=(native_win_random_access_handle&&) noexcept = default;

    /// Copy construction is disabled; the handle is uniquely owned.
    native_win_random_access_handle(
        native_win_random_access_handle const&) = delete;
    /// Copy assignment is disabled; the handle is uniquely owned.
    native_win_random_access_handle&
    operator=(native_win_random_access_handle const&) = delete;

    /** Asynchronously read at the given offset.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Unlike @ref win_random_access_handle::read_some_at,
        it skips the `is_open()` check. A closed handle still completes
        with `errc::bad_file_descriptor`. Do not call it on a moved-from
        object.

        @param offset The byte offset to read at.
        @param buffers The buffers to read into.

        @return An awaitable yielding the error code and the byte count read.
    */
    template<capy::MutableBufferSequence MB>
    [[nodiscard]] auto read_some_at(std::uint64_t offset, MB const& buffers)
    {
        return native_read_at_awaitable<MB>(*this, offset, buffers);
    }

    /** Asynchronously write at the given offset.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Unlike @ref win_random_access_handle::write_some_at,
        it skips the `is_open()` check. A closed handle still completes
        with `errc::bad_file_descriptor`. Do not call it on a moved-from
        object.

        @param offset The byte offset to write at.
        @param buffers The buffer data to write.

        @return An awaitable yielding the error code and the byte count written.
    */
    template<capy::ConstBufferSequence CB>
    [[nodiscard]] auto write_some_at(std::uint64_t offset, CB const& buffers)
    {
        return native_write_at_awaitable<CB>(*this, offset, buffers);
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP || BOOST_COROSIO_MRDOCS

#endif // BOOST_COROSIO_NATIVE_NATIVE_WIN_RANDOM_ACCESS_HANDLE_HPP
