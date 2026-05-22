//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_LOCAL_DATAGRAM_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_LOCAL_DATAGRAM_SOCKET_HPP

#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/backend.hpp>

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
#include <boost/corosio/native/detail/iocp/win_local_dgram_service.hpp>
#endif
#endif // !BOOST_COROSIO_MRDOCS

namespace boost::corosio {

/** An asynchronous Unix datagram socket with devirtualized I/O.

    This class template inherits from @ref local_datagram_socket
    and shadows the async operations (`send_to`, `recv_from`,
    `connect`, `send`, `recv`) with versions that call the backend
    implementation directly, allowing the compiler to inline
    through the entire call chain.

    Non-async operations (`open`, `close`, `cancel`, `bind`,
    socket options) remain unchanged and dispatch through the
    compiled library.

    A `native_local_datagram_socket` IS-A `local_datagram_socket`
    and can be passed to any function expecting
    `local_datagram_socket&`, in which case virtual dispatch is
    used transparently.

    @tparam Backend A backend tag value (e.g., `epoll`) whose type
        provides the concrete implementation types.

    @par Thread Safety
    Same as @ref local_datagram_socket.

    @par Example
    @code
    #include <boost/corosio/native/native_local_datagram_socket.hpp>

    native_io_context<epoll> ctx;
    native_local_datagram_socket<epoll> s(ctx);
    s.open();
    s.bind(local_endpoint("/tmp/recv.sock"));
    char buf[1024];
    local_endpoint sender;
    auto [ec, n] = co_await s.recv_from(
        capy::mutable_buffer(buf, sizeof(buf)), sender);
    @endcode

    @see local_datagram_socket, epoll_t, iocp_t
*/
template<auto Backend>
class native_local_datagram_socket : public local_datagram_socket
{
    using backend_type = decltype(Backend);
    using impl_type    = typename backend_type::local_datagram_socket_type;
    using service_type = typename backend_type::local_datagram_service_type;

    impl_type& get_impl() noexcept
    {
        return *static_cast<impl_type*>(h_.get());
    }

    template<class ConstBufferSequence>
    struct native_send_to_awaitable
    {
        native_local_datagram_socket& self_;
        ConstBufferSequence buffers_;
        corosio::local_endpoint dest_;
        int flags_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        native_send_to_awaitable(
            native_local_datagram_socket& self,
            ConstBufferSequence buffers,
            corosio::local_endpoint dest,
            int flags) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
            , dest_(dest)
            , flags_(flags)
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
            return self_.get_impl().send_to(
                h, env->executor, buffers_, dest_, flags_,
                token_, &ec_, &bytes_transferred_);
        }
    };

    template<class MutableBufferSequence>
    struct native_recv_from_awaitable
    {
        native_local_datagram_socket& self_;
        MutableBufferSequence buffers_;
        corosio::local_endpoint& source_;
        int flags_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        native_recv_from_awaitable(
            native_local_datagram_socket& self,
            MutableBufferSequence buffers,
            corosio::local_endpoint& source,
            int flags) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
            , source_(source)
            , flags_(flags)
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
            return self_.get_impl().recv_from(
                h, env->executor, buffers_, &source_, flags_,
                token_, &ec_, &bytes_transferred_);
        }
    };

    struct native_wait_awaitable
    {
        native_local_datagram_socket& self_;
        wait_type w_;
        std::stop_token token_;
        mutable std::error_code ec_;

        native_wait_awaitable(
            native_local_datagram_socket& self, wait_type w) noexcept
            : self_(self)
            , w_(w)
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
            return self_.get_impl().wait(
                h, env->executor, w_, token_, &ec_);
        }
    };

    struct native_connect_awaitable
    {
        native_local_datagram_socket& self_;
        corosio::local_endpoint endpoint_;
        std::stop_token token_;
        mutable std::error_code ec_;

        native_connect_awaitable(
            native_local_datagram_socket& self,
            corosio::local_endpoint ep) noexcept
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

    template<class ConstBufferSequence>
    struct native_send_awaitable
    {
        native_local_datagram_socket& self_;
        ConstBufferSequence buffers_;
        int flags_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        native_send_awaitable(
            native_local_datagram_socket& self,
            ConstBufferSequence buffers,
            int flags) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
            , flags_(flags)
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
            return self_.get_impl().send(
                h, env->executor, buffers_, flags_,
                token_, &ec_, &bytes_transferred_);
        }
    };

    template<class MutableBufferSequence>
    struct native_recv_awaitable
    {
        native_local_datagram_socket& self_;
        MutableBufferSequence buffers_;
        int flags_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        native_recv_awaitable(
            native_local_datagram_socket& self,
            MutableBufferSequence buffers,
            int flags) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
            , flags_(flags)
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
            return self_.get_impl().recv(
                h, env->executor, buffers_, flags_,
                token_, &ec_, &bytes_transferred_);
        }
    };

public:
    /** Construct a native socket from an execution context.

        @param ctx The execution context that will own this socket.
    */
    explicit native_local_datagram_socket(capy::execution_context& ctx)
        : local_datagram_socket(create_handle<service_type>(ctx))
    {
    }

    /** Construct a native socket from an executor.

        @param ex The executor whose context will own the socket.
    */
    template<class Ex>
        requires(!std::same_as<
                 std::remove_cvref_t<Ex>,
                 native_local_datagram_socket>) &&
        capy::Executor<Ex>
    explicit native_local_datagram_socket(Ex const& ex)
        : native_local_datagram_socket(ex.context())
    {
    }

    /// Move construct.
    native_local_datagram_socket(native_local_datagram_socket&&) noexcept =
        default;

    /// Move assign.
    native_local_datagram_socket&
    operator=(native_local_datagram_socket&&) noexcept = default;

    native_local_datagram_socket(native_local_datagram_socket const&) = delete;
    native_local_datagram_socket&
    operator=(native_local_datagram_socket const&) = delete;

    /** Send a datagram to the specified destination.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref local_datagram_socket::send_to.
    */
    template<capy::ConstBufferSequence CB>
    auto send_to(
        CB const& buffers,
        corosio::local_endpoint dest,
        corosio::message_flags flags)
    {
        if (!is_open())
            detail::throw_logic_error("send_to: socket not open");
        return native_send_to_awaitable<CB>(
            *this, buffers, dest, static_cast<int>(flags));
    }

    /// @overload
    template<capy::ConstBufferSequence CB>
    auto send_to(CB const& buffers, corosio::local_endpoint dest)
    {
        return send_to(buffers, dest, corosio::message_flags::none);
    }

    /** Receive a datagram and capture the sender's endpoint.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref local_datagram_socket::recv_from.
    */
    template<capy::MutableBufferSequence MB>
    auto recv_from(
        MB const& buffers,
        corosio::local_endpoint& source,
        corosio::message_flags flags)
    {
        if (!is_open())
            detail::throw_logic_error("recv_from: socket not open");
        return native_recv_from_awaitable<MB>(
            *this, buffers, source, static_cast<int>(flags));
    }

    /// @overload
    template<capy::MutableBufferSequence MB>
    auto recv_from(MB const& buffers, corosio::local_endpoint& source)
    {
        return recv_from(buffers, source, corosio::message_flags::none);
    }

    /** Asynchronously connect to set the default peer.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref local_datagram_socket::connect.

        If the socket is not already open, it is opened automatically.
    */
    auto connect(corosio::local_endpoint ep)
    {
        if (!is_open())
            open();
        return native_connect_awaitable(*this, ep);
    }

    /** Send a datagram to the connected peer.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref local_datagram_socket::send.
    */
    template<capy::ConstBufferSequence CB>
    auto send(CB const& buffers, corosio::message_flags flags)
    {
        if (!is_open())
            detail::throw_logic_error("send: socket not open");
        return native_send_awaitable<CB>(
            *this, buffers, static_cast<int>(flags));
    }

    /// @overload
    template<capy::ConstBufferSequence CB>
    auto send(CB const& buffers)
    {
        return send(buffers, corosio::message_flags::none);
    }

    /** Receive a datagram from the connected peer.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref local_datagram_socket::recv.
    */
    template<capy::MutableBufferSequence MB>
    auto recv(MB const& buffers, corosio::message_flags flags)
    {
        if (!is_open())
            detail::throw_logic_error("recv: socket not open");
        return native_recv_awaitable<MB>(
            *this, buffers, static_cast<int>(flags));
    }

    /// @overload
    template<capy::MutableBufferSequence MB>
    auto recv(MB const& buffers)
    {
        return recv(buffers, corosio::message_flags::none);
    }

    /** Asynchronously wait for the socket to be ready.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref local_datagram_socket::wait.

        @param w The wait direction (read, write, or error).

        @return An awaitable yielding `io_result<>`.
    */
    [[nodiscard]] auto wait(wait_type w)
    {
        return native_wait_awaitable(*this, w);
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_NATIVE_NATIVE_LOCAL_DATAGRAM_SOCKET_HPP
