//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_WIN_OBJECT_HANDLE_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_WIN_OBJECT_HANDLE_HPP

#include <boost/corosio/detail/op_base.hpp>
#include <boost/corosio/win_object_handle.hpp>
#include <boost/corosio/backend.hpp>

#if BOOST_COROSIO_HAS_IOCP || defined(BOOST_COROSIO_MRDOCS)

#ifndef BOOST_COROSIO_MRDOCS
#include <boost/corosio/native/detail/iocp/win_object_handle_service.hpp>
#endif

namespace boost::corosio {

/** Waits on an already-open Windows kernel object, calling the backend directly.

    This class template inherits from @ref win_object_handle and
    shadows the async operation (`wait`) with a version that calls
    the backend implementation directly. This lets the compiler
    inline through the entire call chain.

    Non-async operations (`assign`, `release`, `close`, `cancel`)
    remain unchanged and dispatch through the compiled library.

    A `native_win_object_handle` IS-A `win_object_handle` and can be
    passed to any function expecting `win_object_handle&`, in which
    case virtual dispatch is used transparently.

    @tparam Backend A backend tag value (e.g., `iocp`) whose type
        provides the concrete implementation types.

    @par Thread Safety
    Same as @ref win_object_handle.

    @see win_object_handle, iocp_t
*/
template<auto Backend>
class native_win_object_handle : public win_object_handle
{
    using backend_type = decltype(Backend);
    using impl_type    = typename backend_type::object_handle_type;
    using service_type = typename backend_type::object_handle_service_type;

    impl_type& get_impl() noexcept
    {
        return *static_cast<impl_type*>(h_.get());
    }

    struct native_wait_awaitable : detail::void_op_base<native_wait_awaitable>
    {
        native_win_object_handle& self_;

        explicit native_wait_awaitable(native_win_object_handle& self) noexcept
            : self_(self)
        {
        }

        std::coroutine_handle<>
        dispatch(capy::continuation& cont, capy::executor_ref ex) const
        {
            return self_.get_impl().wait(cont, ex, this->token_, &this->ec_);
        }
    };

public:
    /** Construct a `native_win_object_handle` from an execution context.

        @param ctx The execution context that owns this object.
    */
    explicit native_win_object_handle(capy::execution_context& ctx)
        : win_object_handle(handle(ctx, ctx.use_service<service_type>()))
    {
    }

    /** Construct a `native_win_object_handle` from an executor.

        The overload excludes `native_win_object_handle` itself so
        that it cannot displace the move constructor.

        @tparam Ex A type satisfying `capy::Executor`.
        @param ex The executor whose context owns this object.
    */
    template<class Ex>
        requires(!std::same_as<
                    std::remove_cvref_t<Ex>,
                    native_win_object_handle>) &&
        capy::Executor<Ex>
    explicit native_win_object_handle(Ex const& ex)
        : native_win_object_handle(ex.context())
    {
    }

    /** Transfer ownership of the handle from the source.

        After the move, the source is in a moved-from state and may
        only be destroyed or assigned to.

        @pre No awaitables returned by the source's methods exist.
    */
    native_win_object_handle(native_win_object_handle&&) noexcept = default;

    /** Close any held handle and transfer ownership from the source.

        After the move, the source is in a moved-from state and may
        only be destroyed or assigned to.

        @return `*this`.
        @pre No awaitables returned by either object's methods exist.
    */
    native_win_object_handle&
    operator=(native_win_object_handle&&) noexcept = default;

    /// Copy construction is disabled; the handle is uniquely owned.
    native_win_object_handle(native_win_object_handle const&) = delete;
    /// Copy assignment is disabled; the handle is uniquely owned.
    native_win_object_handle&
    operator=(native_win_object_handle const&) = delete;

    /** Wait for the object to become signaled.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref win_object_handle::wait.

        @return An awaitable yielding `io_result<>`.
    */
    [[nodiscard]] auto wait()
    {
        return native_wait_awaitable(*this);
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP || BOOST_COROSIO_MRDOCS

#endif // BOOST_COROSIO_NATIVE_NATIVE_WIN_OBJECT_HANDLE_HPP
