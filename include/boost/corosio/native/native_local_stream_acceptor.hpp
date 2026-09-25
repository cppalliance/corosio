//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_LOCAL_STREAM_ACCEPTOR_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_LOCAL_STREAM_ACCEPTOR_HPP

#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/native/native_local_stream_socket.hpp>
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

#if BOOST_COROSIO_HAS_URING
#include <boost/corosio/native/detail/uring/uring_types.hpp>
#endif

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_local_stream_acceptor_service.hpp>
#endif
#endif // !BOOST_COROSIO_MRDOCS

namespace boost::corosio {

/** Accepts Unix domain stream connections, calling the backend directly.

    This class template inherits from @ref local_stream_acceptor. It
    shadows both `accept` overloads (the peer-reference form and the
    move-return form) with versions that call the backend implementation
    directly. The compiler can then inline through the entire call
    chain. The move-return form yields a
    @ref native_local_stream_socket so subsequent I/O on the peer
    is also devirtualized.

    Non-async operations (`listen`, `close`, `cancel`) remain
    unchanged and dispatch through the compiled library.

    A `native_local_stream_acceptor` IS-A `local_stream_acceptor`
    and can be passed to any function expecting
    `local_stream_acceptor&`.

    @tparam Backend A backend tag value (e.g., `epoll`).

    @par Thread Safety
    Same as @ref local_stream_acceptor.

    @see local_stream_acceptor, epoll_t, iocp_t
*/
template<auto Backend>
class native_local_stream_acceptor : public local_stream_acceptor
{
    using backend_type = decltype(Backend);
    using impl_type    = typename backend_type::local_stream_acceptor_type;
    using service_type =
        typename backend_type::local_stream_acceptor_service_type;

    impl_type& get_impl() noexcept
    {
        return *static_cast<impl_type*>(h_.get());
    }

    struct native_wait_awaitable : detail::void_op_base<native_wait_awaitable>
    {
        native_local_stream_acceptor& acc_;
        wait_type w_;

        native_wait_awaitable(
            native_local_stream_acceptor& acc, wait_type w) noexcept
            : acc_(acc)
            , w_(w)
        {
        }

        std::coroutine_handle<>
        dispatch(std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return acc_.get_impl().wait(h, ex, w_, this->token_, &this->ec_);
        }
    };

    struct native_accept_awaitable
        : detail::void_op_base<native_accept_awaitable>
    {
        native_local_stream_acceptor& acc_;
        local_stream_socket& peer_;
        mutable io_object::implementation* peer_impl_ = nullptr;

        native_accept_awaitable(
            native_local_stream_acceptor& acc,
            local_stream_socket& peer) noexcept
            : acc_(acc)
            , peer_(peer)
        {
        }

        [[nodiscard]] capy::io_result<> await_resume() const noexcept
        {
            if (!this->ec_)
                acc_.reset_peer_impl(peer_, peer_impl_);
            return {this->ec_};
        }

        std::coroutine_handle<>
        dispatch(std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return acc_.get_impl().accept(
                h, ex, this->token_, &this->ec_, &peer_impl_);
        }
    };

    struct native_move_accept_awaitable
        : detail::void_op_base<native_move_accept_awaitable>
    {
        native_local_stream_acceptor& acc_;
        mutable io_object::implementation* peer_impl_ = nullptr;

        explicit native_move_accept_awaitable(
            native_local_stream_acceptor& acc) noexcept
            : acc_(acc)
        {
        }

        [[nodiscard]] capy::io_result<native_local_stream_socket<Backend>>
        await_resume() const noexcept
        {
            if (this->ec_ || !peer_impl_)
                return {
                    this->ec_,
                    native_local_stream_socket<Backend>(acc_.context())};

            native_local_stream_socket<Backend> peer(acc_.context());
            acc_.reset_peer_impl(peer, peer_impl_);
            return {this->ec_, std::move(peer)};
        }

        std::coroutine_handle<>
        dispatch(std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return acc_.get_impl().accept(
                h, ex, this->token_, &this->ec_, &peer_impl_);
        }
    };

public:
    /** Construct a native acceptor from an execution context.

        @param ctx The execution context that owns this acceptor.
    */
    explicit native_local_stream_acceptor(capy::execution_context& ctx)
        : local_stream_acceptor(create_handle<service_type>(ctx), ctx)
    {
    }

    /** Construct a native acceptor from an executor.

        @param ex The executor whose context owns the acceptor.
    */
    template<class Ex>
        requires(!std::same_as<
                    std::remove_cvref_t<Ex>,
                    native_local_stream_acceptor>) &&
        capy::Executor<Ex>
    explicit native_local_stream_acceptor(Ex const& ex)
        : native_local_stream_acceptor(ex.context())
    {
    }

    /// Move construct.
    native_local_stream_acceptor(native_local_stream_acceptor&&) noexcept =
        default;

    /// Move assign.
    native_local_stream_acceptor&
    operator=(native_local_stream_acceptor&&) noexcept = default;

    /// Copy construction is disabled; the handle is uniquely owned.
    native_local_stream_acceptor(native_local_stream_acceptor const&) = delete;
    /// Copy assignment is disabled; the handle is uniquely owned.
    native_local_stream_acceptor&
    operator=(native_local_stream_acceptor const&) = delete;

    /** Asynchronously accept an incoming connection.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref local_stream_acceptor::accept.

        @param peer The socket to receive the accepted connection.

        @return An awaitable yielding `io_result<>`.

        A closed acceptor reports `errc::bad_file_descriptor`.

        Both this acceptor and @p peer must outlive the returned
        awaitable.
    */
    [[nodiscard]] auto accept(local_stream_socket& peer)
    {
        native_accept_awaitable aw(*this, peer);
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /** Asynchronously accept an incoming connection, returning the peer.

        Calls the backend implementation directly, bypassing virtual
        dispatch. The accepted peer is returned as a
        @ref native_local_stream_socket so that subsequent I/O on it
        is also devirtualized.

        @return An awaitable yielding
            `io_result<native_local_stream_socket<Backend>>`.

        A closed acceptor reports `errc::bad_file_descriptor`.

        @throws std::logic_error If the acceptor is moved-from.

        This acceptor must outlive the returned awaitable.
    */
    [[nodiscard]] auto accept()
    {
        // The awaitable builds the peer from context(), which a
        // moved-from acceptor no longer has.
        if (!h_)
            detail::throw_logic_error("accept: acceptor moved-from");
        native_move_accept_awaitable aw(*this);
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /** Asynchronously wait for the acceptor to be ready.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref local_stream_acceptor::wait.

        @param w The wait direction (typically `wait_type::read`).

        @return An awaitable yielding `io_result<>`.
    */
    [[nodiscard]] auto wait(wait_type w)
    {
        return native_wait_awaitable(*this, w);
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_NATIVE_NATIVE_LOCAL_STREAM_ACCEPTOR_HPP
