//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_TCP_ACCEPTOR_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_TCP_ACCEPTOR_HPP

#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/backend.hpp>
#include <boost/corosio/detail/op_base.hpp>

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

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_tcp_acceptor_service.hpp>
#endif

#if BOOST_COROSIO_HAS_URING
#include <boost/corosio/native/detail/uring/uring_types.hpp>
#endif
#endif // !BOOST_COROSIO_MRDOCS

namespace boost::corosio {

/** Accepts TCP connections, calling the backend directly.

    This class template inherits from @ref tcp_acceptor. It shadows the
    `accept` operation with a version that calls the backend
    implementation directly. The compiler can then inline through the
    entire call chain.

    Non-async operations (`listen`, `close`, `cancel`) remain
    unchanged and dispatch through the compiled library.

    A `native_tcp_acceptor` IS-A `tcp_acceptor` and can be passed
    to any function expecting `tcp_acceptor&`.

    @tparam Backend A backend tag value (e.g., `epoll`).

    @par Thread Safety
    Same as @ref tcp_acceptor.

    @see @ref tcp_acceptor, @ref epoll_t, @ref iocp_t
*/
template<auto Backend>
class native_tcp_acceptor : public tcp_acceptor
{
    using backend_type = decltype(Backend);
    using impl_type    = typename backend_type::tcp_acceptor_type;
    using service_type = typename backend_type::tcp_acceptor_service_type;

    impl_type& get_impl() noexcept
    {
        return *static_cast<impl_type*>(h_.get());
    }

    struct native_wait_awaitable : detail::void_op_base<native_wait_awaitable>
    {
        native_tcp_acceptor& acc_;
        wait_type w_;

        native_wait_awaitable(native_tcp_acceptor& acc, wait_type w) noexcept
            : acc_(acc)
            , w_(w)
        {
        }

        std::coroutine_handle<>
        dispatch(capy::continuation& cont, capy::executor_ref ex) const
        {
            return acc_.get_impl().wait(cont, ex, w_, this->token_, &this->ec_);
        }
    };

    struct native_accept_awaitable
        : detail::void_op_base<native_accept_awaitable>
    {
        native_tcp_acceptor& acc_;
        tcp_socket& peer_;
        mutable io_object::implementation* peer_impl_ = nullptr;

        native_accept_awaitable(
            native_tcp_acceptor& acc, tcp_socket& peer) noexcept
            : acc_(acc)
            , peer_(peer)
        {
        }

        // A peer accepted for an awaiter that never resumed is closed
        // here, or nothing would own it.
        ~native_accept_awaitable()
        {
            if (peer_impl_)
                discard_peer(acc_, peer_impl_);
        }

        [[nodiscard]] capy::results<> await_resume() const noexcept
        {
            if (!this->ec_)
                acc_.reset_peer_impl(peer_, std::exchange(peer_impl_, nullptr));
            return {this->ec_};
        }

        std::coroutine_handle<>
        dispatch(capy::continuation& cont, capy::executor_ref ex) const
        {
            return acc_.get_impl().accept(
                cont, ex, this->token_, &this->ec_, &peer_impl_);
        }
    };

    struct native_accept_value_awaitable
        : detail::void_op_base<native_accept_value_awaitable>
    {
        native_tcp_acceptor& acc_;
        mutable io_object::implementation* peer_impl_ = nullptr;

        explicit native_accept_value_awaitable(
            native_tcp_acceptor& acc) noexcept
            : acc_(acc)
        {
        }

        // A peer accepted for an awaiter that never resumed is closed
        // here, or nothing would own it.
        ~native_accept_value_awaitable()
        {
            if (peer_impl_)
                discard_peer(acc_, peer_impl_);
        }

        [[nodiscard]] capy::results<tcp_socket> await_resume() noexcept
        {
            return take_peer(acc_, this->ec_, peer_impl_);
        }

        std::coroutine_handle<>
        dispatch(capy::continuation& cont, capy::executor_ref ex) const
        {
            return acc_.get_impl().accept(
                cont, ex, this->token_, &this->ec_, &peer_impl_);
        }
    };

public:
    /** Construct a native acceptor from an execution context.

        @param ctx The execution context that owns this acceptor.
    */
    explicit native_tcp_acceptor(capy::execution_context& ctx)
        : tcp_acceptor(handle(ctx, ctx.use_service<service_type>()))
    {
    }

    /** Construct a native acceptor from an executor.

        @param ex The executor whose context owns the acceptor.

        @tparam Ex A type satisfying @ref capy::Executor. Must not
            be `native_tcp_acceptor` itself (disables implicit
            conversion from move).
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, native_tcp_acceptor>) &&
        capy::Executor<Ex>
    explicit native_tcp_acceptor(Ex const& ex)
        : native_tcp_acceptor(ex.context())
    {
    }

    /** Construct a native acceptor that is open, bound, and listening.

        Creates a fully bound listening acceptor in a single
        expression, throwing the codes the piecewise `open()` +
        `set_option()` + `bind()` + `listen()` path reports. The
        address family is deduced from @p ep, and address reuse is
        configured as by the @ref tcp_acceptor convenience
        constructor.

        @param ctx The execution context that owns this acceptor.
        @param ep The local endpoint to bind to.
        @param backlog The maximum pending connection queue length.

        @throws std::system_error on open, configuration, bind, or
            listen failure.
    */
    native_tcp_acceptor(
        capy::execution_context& ctx, endpoint ep, int backlog = 128)
        : tcp_acceptor(
              handle(ctx, ctx.use_service<service_type>()), ep, backlog)
    {
    }

    /** Construct a native acceptor that is open, bound, and listening.

        @param ex The executor whose context owns the acceptor.
        @param ep The local endpoint to bind to.
        @param backlog The maximum pending connection queue length.

        @tparam Ex A type satisfying @ref capy::Executor.

        @throws std::system_error on open, configuration, bind, or
            listen failure.
    */
    template<class Ex>
        requires capy::Executor<Ex>
    native_tcp_acceptor(Ex const& ex, endpoint ep, int backlog = 128)
        : native_tcp_acceptor(ex.context(), ep, backlog)
    {
    }

    /** Move construct.

        @param other The acceptor to move from.

        @pre No awaitables returned by @p other's methods exist.
        @pre The execution context associated with @p other must
            outlive this acceptor.
    */
    native_tcp_acceptor(native_tcp_acceptor&&) noexcept = default;

    /** Move assign.

        @param other The acceptor to move from.

        @pre No awaitables returned by either `*this` or @p other's
            methods exist.
        @pre The execution context associated with @p other must
            outlive this acceptor.
    */
    native_tcp_acceptor& operator=(native_tcp_acceptor&&) noexcept = default;

    /// Copy construction is disabled; the handle is uniquely owned.
    native_tcp_acceptor(native_tcp_acceptor const&) = delete;
    /// Copy assignment is disabled; the handle is uniquely owned.
    native_tcp_acceptor& operator=(native_tcp_acceptor const&) = delete;

    /** Asynchronously accept an incoming connection.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref tcp_acceptor::accept.

        @param peer The socket to receive the accepted connection.

        @return An awaitable yielding `results<>`.

        A closed or moved-from acceptor reports
        `errc::bad_file_descriptor`.

        Both this acceptor and @p peer must outlive the returned
        awaitable.
    */
    [[nodiscard]] auto accept(tcp_socket& peer)
    {
        native_accept_awaitable aw(*this, peer);
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /** Asynchronously accept an incoming connection, returning the peer.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref tcp_acceptor::accept().

        @return An awaitable yielding `results<tcp_socket>`.

        A closed or moved-from acceptor reports
        `errc::bad_file_descriptor`.
        On failure the returned socket is default-constructed and not
        open. It has no context, so it may only be destroyed, assigned,
        or checked with `is_open()`.

        This acceptor must outlive the returned awaitable.
    */
    [[nodiscard]] auto accept()
    {
        native_accept_value_awaitable aw(*this);
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /** Asynchronously wait for the acceptor to be ready.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref tcp_acceptor::wait.

        @param w The wait direction (typically `wait_type::read`).

        @return An awaitable yielding `results<>`.
    */
    [[nodiscard]] auto wait(wait_type w)
    {
        return native_wait_awaitable(*this, w);
    }
};

} // namespace boost::corosio

#endif
