//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_POSIX_STREAM_DESCRIPTOR_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_POSIX_STREAM_DESCRIPTOR_HPP

#include <boost/corosio/detail/op_base.hpp>
#include <boost/corosio/posix_stream_descriptor.hpp>
#include <boost/corosio/backend.hpp>

#if BOOST_COROSIO_POSIX || defined(BOOST_COROSIO_MRDOCS)

#ifndef BOOST_COROSIO_MRDOCS
#if BOOST_COROSIO_HAS_EPOLL
#include <boost/corosio/native/detail/epoll/epoll_types.hpp>
#endif

#if BOOST_COROSIO_HAS_SELECT
#include <boost/corosio/native/detail/select/select_types.hpp>
#endif

#if BOOST_COROSIO_HAS_KQUEUE
#include <boost/corosio/native/detail/kqueue/kqueue_types.hpp>
#endif

#if BOOST_COROSIO_HAS_URING
// uring_types.hpp does not declare the descriptor: it was added
// after that header, in its own pair of files.
#include <boost/corosio/native/detail/uring/uring_descriptor_service.hpp>
#endif
#endif // !BOOST_COROSIO_MRDOCS

namespace boost::corosio {

/** Drives an already-open POSIX descriptor, calling the backend directly.

    This class template inherits from @ref posix_stream_descriptor and
    shadows the async operations (`read_some`, `write_some`, `wait`)
    with versions that call the backend implementation directly.
    This lets the compiler inline through the entire call chain.

    Non-async operations (`assign`, `release`, `close`, `cancel`)
    remain unchanged and dispatch through the compiled library.

    A `native_posix_stream_descriptor` IS-A `posix_stream_descriptor` and can be
    passed to any function expecting `posix_stream_descriptor&` or
    `io_stream&`, in which case virtual dispatch is used
    transparently.

    @tparam Backend A backend tag value (e.g., `epoll`) whose type
        provides the concrete implementation types.

    @par Thread Safety
    Same as @ref posix_stream_descriptor.

    @par Example
    @par !example assign_and_wait

    @see posix_stream_descriptor, epoll_t, kqueue_t
*/
template<auto Backend>
class native_posix_stream_descriptor : public posix_stream_descriptor
{
    using backend_type = decltype(Backend);
    using impl_type    = typename backend_type::stream_descriptor_type;
    using service_type = typename backend_type::stream_descriptor_service_type;

    impl_type& get_impl() noexcept
    {
        return *static_cast<impl_type*>(h_.get());
    }

    template<class MutableBufferSequence>
    struct native_read_awaitable
        : detail::bytes_op_base<native_read_awaitable<MutableBufferSequence>>
    {
        native_posix_stream_descriptor& self_;
        MutableBufferSequence buffers_;

        native_read_awaitable(
            native_posix_stream_descriptor& self,
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
        native_posix_stream_descriptor& self_;
        ConstBufferSequence buffers_;

        native_write_awaitable(
            native_posix_stream_descriptor& self,
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

    struct native_wait_awaitable : detail::void_op_base<native_wait_awaitable>
    {
        native_posix_stream_descriptor& self_;
        wait_type w_;

        native_wait_awaitable(
            native_posix_stream_descriptor& self, wait_type w) noexcept
            : self_(self)
            , w_(w)
        {
        }

        std::coroutine_handle<>
        dispatch(capy::continuation& cont, capy::executor_ref ex) const
        {
            return self_.get_impl().wait(
                cont, ex, w_, this->token_, &this->ec_);
        }
    };

public:
    /** Construct a `native_posix_stream_descriptor` from an execution
        context.

        @param ctx The execution context that owns this object.
    */
    explicit native_posix_stream_descriptor(capy::execution_context& ctx)
        : io_object(handle(ctx, ctx.use_service<service_type>()))
    {
    }

    /** Construct a `native_posix_stream_descriptor` from an executor.

        The overload excludes `native_posix_stream_descriptor` itself so
        that it cannot displace the move constructor.

        @tparam Ex A type satisfying `capy::Executor`.
        @param ex The executor whose context owns this object.
    */
    template<class Ex>
        requires(!std::same_as<
                    std::remove_cvref_t<Ex>,
                    native_posix_stream_descriptor>) &&
        capy::Executor<Ex>
    explicit native_posix_stream_descriptor(Ex const& ex)
        : native_posix_stream_descriptor(ex.context())
    {
    }

    /** Transfer ownership of the descriptor from the source.

        After the move, the source is in a moved-from state and may
        only be destroyed or assigned to.

        @pre No awaitables returned by the source's methods exist.
    */
    native_posix_stream_descriptor(native_posix_stream_descriptor&&) noexcept =
        default;

    /** Close any held descriptor and transfer ownership from the source.

        After the move, the source is in a moved-from state and may
        only be destroyed or assigned to.

        @return `*this`.
        @pre No awaitables returned by either object's methods exist.
    */
    native_posix_stream_descriptor&
    operator=(native_posix_stream_descriptor&&) noexcept = default;

    /// Copy construction is disabled; the handle is uniquely owned.
    native_posix_stream_descriptor(native_posix_stream_descriptor const&) =
        delete;
    /// Copy assignment is disabled; the handle is uniquely owned.
    native_posix_stream_descriptor&
    operator=(native_posix_stream_descriptor const&) = delete;

    /** Asynchronously read data from the descriptor.

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

    /** Asynchronously write data to the descriptor.

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

    /** Wait for readiness without transferring bytes.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref posix_stream_descriptor::wait.

        @param w The direction to wait on.

        @return An awaitable yielding `io_result<>`.
    */
    [[nodiscard]] auto wait(wait_type w)
    {
        return native_wait_awaitable(*this, w);
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX || BOOST_COROSIO_MRDOCS

#endif // BOOST_COROSIO_NATIVE_NATIVE_POSIX_STREAM_DESCRIPTOR_HPP
