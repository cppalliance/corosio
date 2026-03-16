//
// Copyright (c) 2026 Steve Gerbino
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

#ifndef BOOST_COROSIO_MRDOCS
#if BOOST_COROSIO_HAS_EPOLL
#include <boost/corosio/native/detail/epoll/epoll_tcp_acceptor_service.hpp>
#endif

#if BOOST_COROSIO_HAS_SELECT
#include <boost/corosio/native/detail/select/select_tcp_acceptor_service.hpp>
#endif

#if BOOST_COROSIO_HAS_KQUEUE
#include <boost/corosio/native/detail/kqueue/kqueue_tcp_acceptor_service.hpp>
#endif

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_tcp_acceptor_service.hpp>
#endif
#endif // !BOOST_COROSIO_MRDOCS

namespace boost::corosio {

/** An asynchronous TCP acceptor with devirtualized accept operations.

    This class template inherits from @ref tcp_acceptor and shadows
    the `accept` operation with a version that calls the backend
    implementation directly, allowing the compiler to inline through
    the entire call chain.

    Non-async operations (`listen`, `close`, `cancel`) remain
    unchanged and dispatch through the compiled library.

    A `native_tcp_acceptor` IS-A `tcp_acceptor` and can be passed
    to any function expecting `tcp_acceptor&`.

    @tparam Backend A backend tag value (e.g., `epoll`).

    @par Thread Safety
    Same as @ref tcp_acceptor.

    @see tcp_acceptor, epoll_t, iocp_t
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

    struct native_accept_awaitable
    {
        native_tcp_acceptor& acc_;
        tcp_socket& peer_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable io_object::implementation* peer_impl_ = nullptr;

        native_accept_awaitable(
            native_tcp_acceptor& acc, tcp_socket& peer) noexcept
            : acc_(acc)
            , peer_(peer)
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        capy::io_result<> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(std::errc::operation_canceled)};
            if (!ec_)
                acc_.reset_peer_impl(peer_, peer_impl_);
            return {ec_};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            return acc_.get_impl().accept(
                h, env->executor, token_, &ec_, &peer_impl_);
        }
    };

public:
    /** Construct a native acceptor from an execution context.

        @param ctx The execution context that will own this acceptor.
    */
    explicit native_tcp_acceptor(capy::execution_context& ctx)
        : tcp_acceptor(create_handle<service_type>(ctx))
    {
    }

    /** Construct a native acceptor from an executor.

        @param ex The executor whose context will own the acceptor.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, native_tcp_acceptor>) &&
        capy::Executor<Ex>
    explicit native_tcp_acceptor(Ex const& ex)
        : native_tcp_acceptor(ex.context())
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

    native_tcp_acceptor(native_tcp_acceptor const&)            = delete;
    native_tcp_acceptor& operator=(native_tcp_acceptor const&) = delete;

    /** Asynchronously accept an incoming connection.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref tcp_acceptor::accept.

        @param peer The socket to receive the accepted connection.

        @return An awaitable yielding `io_result<>`.

        @throws std::logic_error if the acceptor is not listening.

        Both this acceptor and @p peer must outlive the returned
        awaitable.
    */
    auto accept(tcp_socket& peer)
    {
        if (!is_open())
            detail::throw_logic_error("accept: acceptor not listening");
        return native_accept_awaitable(*this, peer);
    }
};

} // namespace boost::corosio

#endif
