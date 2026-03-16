//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_UDP_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_UDP_SOCKET_HPP

#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/backend.hpp>

#ifndef BOOST_COROSIO_MRDOCS
#if BOOST_COROSIO_HAS_EPOLL
#include <boost/corosio/native/detail/epoll/epoll_udp_service.hpp>
#endif

#if BOOST_COROSIO_HAS_SELECT
#include <boost/corosio/native/detail/select/select_udp_service.hpp>
#endif

#if BOOST_COROSIO_HAS_KQUEUE
#include <boost/corosio/native/detail/kqueue/kqueue_udp_service.hpp>
#endif
#endif // !BOOST_COROSIO_MRDOCS

namespace boost::corosio {

/** An asynchronous UDP socket with devirtualized I/O operations.

    This class template inherits from @ref udp_socket and shadows
    the async operations (`send_to`, `recv_from`) with versions
    that call the backend implementation directly, allowing the
    compiler to inline through the entire call chain.

    Non-async operations (`open`, `close`, `cancel`, `bind`,
    socket options) remain unchanged and dispatch through the
    compiled library.

    A `native_udp_socket` IS-A `udp_socket` and can be passed to
    any function expecting `udp_socket&`, in which case virtual
    dispatch is used transparently.

    @tparam Backend A backend tag value (e.g., `epoll`)
        whose type provides the concrete implementation types.

    @par Thread Safety
    Same as @ref udp_socket.

    @par Example
    @code
    #include <boost/corosio/native/native_udp_socket.hpp>

    native_io_context<epoll> ctx;
    native_udp_socket<epoll> s(ctx);
    s.open();
    s.bind(endpoint(ipv4_address::any(), 9000));
    char buf[1024];
    endpoint sender;
    auto [ec, n] = co_await s.recv_from(
        capy::mutable_buffer(buf, sizeof(buf)), sender);
    @endcode

    @see udp_socket, epoll_t
*/
template<auto Backend>
class native_udp_socket : public udp_socket
{
    using backend_type = decltype(Backend);
    using impl_type    = typename backend_type::udp_socket_type;
    using service_type = typename backend_type::udp_service_type;

    impl_type& get_impl() noexcept
    {
        return *static_cast<impl_type*>(h_.get());
    }

    template<class ConstBufferSequence>
    struct native_send_to_awaitable
    {
        native_udp_socket& self_;
        ConstBufferSequence buffers_;
        endpoint dest_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        native_send_to_awaitable(
            native_udp_socket& self,
            ConstBufferSequence buffers,
            endpoint dest) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
            , dest_(dest)
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
                h, env->executor, buffers_, dest_, token_, &ec_,
                &bytes_transferred_);
        }
    };

    template<class MutableBufferSequence>
    struct native_recv_from_awaitable
    {
        native_udp_socket& self_;
        MutableBufferSequence buffers_;
        endpoint& source_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        native_recv_from_awaitable(
            native_udp_socket& self,
            MutableBufferSequence buffers,
            endpoint& source) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
            , source_(source)
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
                h, env->executor, buffers_, &source_, token_, &ec_,
                &bytes_transferred_);
        }
    };

public:
    /** Construct a native UDP socket from an execution context.

        @param ctx The execution context that will own this socket.
    */
    explicit native_udp_socket(capy::execution_context& ctx)
        : udp_socket(create_handle<service_type>(ctx))
    {
    }

    /** Construct a native UDP socket from an executor.

        @param ex The executor whose context will own the socket.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, native_udp_socket>) &&
        capy::Executor<Ex>
    explicit native_udp_socket(Ex const& ex) : native_udp_socket(ex.context())
    {
    }

    /// Move construct.
    native_udp_socket(native_udp_socket&&) noexcept = default;

    /// Move assign.
    native_udp_socket& operator=(native_udp_socket&&) noexcept = default;

    native_udp_socket(native_udp_socket const&)            = delete;
    native_udp_socket& operator=(native_udp_socket const&) = delete;

    /** Send a datagram to the specified destination.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref udp_socket::send_to.

        @param buffers The buffer sequence containing data to send.
        @param dest The destination endpoint.

        @return An awaitable yielding `(error_code, std::size_t)`.
    */
    template<capy::ConstBufferSequence CB>
    auto send_to(CB const& buffers, endpoint dest)
    {
        if (!is_open())
            detail::throw_logic_error("send_to: socket not open");
        return native_send_to_awaitable<CB>(*this, buffers, dest);
    }

    /** Receive a datagram and capture the sender's endpoint.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref udp_socket::recv_from.

        @param buffers The buffer sequence to receive data into.
        @param source Reference to an endpoint that will be set to
            the sender's address on successful completion.

        @return An awaitable yielding `(error_code, std::size_t)`.
    */
    template<capy::MutableBufferSequence MB>
    auto recv_from(MB const& buffers, endpoint& source)
    {
        if (!is_open())
            detail::throw_logic_error("recv_from: socket not open");
        return native_recv_from_awaitable<MB>(*this, buffers, source);
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_NATIVE_NATIVE_UDP_SOCKET_HPP
