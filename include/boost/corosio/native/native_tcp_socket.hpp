//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_TCP_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_TCP_SOCKET_HPP

#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/backend.hpp>

#if BOOST_COROSIO_HAS_EPOLL
#include <boost/corosio/native/detail/epoll/epoll_socket_service.hpp>
#endif

#if BOOST_COROSIO_HAS_SELECT
#include <boost/corosio/native/detail/select/select_socket_service.hpp>
#endif

#if BOOST_COROSIO_HAS_KQUEUE
#include <boost/corosio/native/detail/kqueue/kqueue_socket_service.hpp>
#endif

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_acceptor_service.hpp>
#endif

namespace boost::corosio {

/** An asynchronous TCP socket with devirtualized I/O operations.

    This class template inherits from @ref tcp_socket and shadows
    the async operations (`read_some`, `write_some`, `connect`) with
    versions that call the backend implementation directly, allowing
    the compiler to inline through the entire call chain.

    Non-async operations (`open`, `close`, `cancel`, socket options)
    remain unchanged and dispatch through the compiled library.

    A `native_tcp_socket` IS-A `tcp_socket` and can be passed to
    any function expecting `tcp_socket&` or `io_stream&`, in which
    case virtual dispatch is used transparently.

    @tparam Backend A backend tag value (e.g., `epoll`,
        `iocp`) whose type provides the concrete implementation
        types.

    @par Thread Safety
    Same as @ref tcp_socket.

    @par Example
    @code
    #include <boost/corosio/native/native_tcp_socket.hpp>

    native_io_context<epoll> ctx;
    native_tcp_socket<epoll> s(ctx);
    s.open();
    auto [ec] = co_await s.connect(ep);
    auto [ec2, n] = co_await s.read_some(buf);
    @endcode

    @see tcp_socket, epoll_t, iocp_t
*/
template<auto Backend>
class native_tcp_socket : public tcp_socket
{
    using backend_type = decltype(Backend);
    using impl_type    = typename backend_type::socket_type;
    using service_type = typename backend_type::socket_service_type;

    impl_type& get_impl() noexcept
    {
        return *static_cast<impl_type*>(h_.get());
    }

    template<class MutableBufferSequence>
    struct native_read_awaitable
    {
        native_tcp_socket& self_;
        MutableBufferSequence buffers_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        native_read_awaitable(
            native_tcp_socket& self, MutableBufferSequence buffers) noexcept
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
        native_tcp_socket& self_;
        ConstBufferSequence buffers_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        native_write_awaitable(
            native_tcp_socket& self, ConstBufferSequence buffers) noexcept
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

    struct native_connect_awaitable
    {
        native_tcp_socket& self_;
        endpoint endpoint_;
        std::stop_token token_;
        mutable std::error_code ec_;

        native_connect_awaitable(native_tcp_socket& self, endpoint ep) noexcept
            : self_(self)
            , endpoint_(ep)
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
            return {ec_};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            return self_.get_impl().connect(
                h, env->executor, endpoint_, token_, &ec_);
        }
    };

public:
    /** Construct a native socket from an execution context.

        @param ctx The execution context that will own this socket.
    */
    explicit native_tcp_socket(capy::execution_context& ctx)
        : io_object(create_handle<service_type>(ctx))
    {
    }

    /** Construct a native socket from an executor.

        @param ex The executor whose context will own the socket.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, native_tcp_socket>) &&
        capy::Executor<Ex>
    explicit native_tcp_socket(Ex const& ex) : native_tcp_socket(ex.context())
    {
    }

    /** Move construct.

        @param other The socket to move from.

        @pre No awaitables returned by @p other's methods exist.
        @pre @p other is not referenced as a peer in any outstanding
            accept awaitable.
        @pre The execution context associated with @p other must
            outlive this socket.
    */
    native_tcp_socket(native_tcp_socket&&) noexcept = default;

    /** Move assign.

        @param other The socket to move from.

        @pre No awaitables returned by either `*this` or @p other's
            methods exist.
        @pre Neither `*this` nor @p other is referenced as a peer in
            any outstanding accept awaitable.
        @pre The execution context associated with @p other must
            outlive this socket.
    */
    native_tcp_socket& operator=(native_tcp_socket&&) noexcept = default;

    native_tcp_socket(native_tcp_socket const&)            = delete;
    native_tcp_socket& operator=(native_tcp_socket const&) = delete;

    /** Asynchronously read data from the socket.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref io_stream::read_some.

        @param buffers The buffer sequence to read into.

        @return An awaitable yielding `(error_code, std::size_t)`.

        This socket must outlive the returned awaitable. The memory
        referenced by @p buffers must remain valid until the operation
        completes.
    */
    template<capy::MutableBufferSequence MB>
    auto read_some(MB const& buffers)
    {
        return native_read_awaitable<MB>(*this, buffers);
    }

    /** Asynchronously write data to the socket.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref io_stream::write_some.

        @param buffers The buffer sequence to write from.

        @return An awaitable yielding `(error_code, std::size_t)`.

        This socket must outlive the returned awaitable. The memory
        referenced by @p buffers must remain valid until the operation
        completes.
    */
    template<capy::ConstBufferSequence CB>
    auto write_some(CB const& buffers)
    {
        return native_write_awaitable<CB>(*this, buffers);
    }

    /** Asynchronously connect to a remote endpoint.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref tcp_socket::connect.

        @param ep The remote endpoint to connect to.

        @return An awaitable yielding `io_result<>`.

        @throws std::logic_error if the socket is not open.

        This socket must outlive the returned awaitable.
    */
    auto connect(endpoint ep)
    {
        if (!is_open())
            detail::throw_logic_error("connect: socket not open");
        return native_connect_awaitable(*this, ep);
    }
};

} // namespace boost::corosio

#endif
