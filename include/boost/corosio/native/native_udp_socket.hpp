//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
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
#include <boost/corosio/native/detail/iocp/win_udp_service.hpp>
#endif
#endif // !BOOST_COROSIO_MRDOCS

namespace boost::corosio {

/** Sends and receives UDP datagrams, calling the backend directly.

    This class template inherits from @ref udp_socket. It shadows the
    async operations (`send_to`, `recv_from`, `connect`, `send`, `recv`)
    with versions that call the backend implementation directly. The
    compiler can then inline through the entire call chain.

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
    @par !example native_udp_socket

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
        : detail::bytes_op_base<native_send_to_awaitable<ConstBufferSequence>>
    {
        native_udp_socket& self_;
        ConstBufferSequence buffers_;
        endpoint dest_;
        int flags_;

        native_send_to_awaitable(
            native_udp_socket& self,
            ConstBufferSequence buffers,
            endpoint dest,
            int flags) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
            , dest_(dest)
            , flags_(flags)
        {
        }

        std::coroutine_handle<>
        dispatch(std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return self_.get_impl().send_to(
                h, ex, buffers_, dest_, flags_, this->token_, &this->ec_,
                &this->bytes_);
        }
    };

    template<class MutableBufferSequence>
    struct native_recv_from_awaitable
        : detail::bytes_op_base<
              native_recv_from_awaitable<MutableBufferSequence>>
    {
        native_udp_socket& self_;
        MutableBufferSequence buffers_;
        endpoint& source_;
        int flags_;

        native_recv_from_awaitable(
            native_udp_socket& self,
            MutableBufferSequence buffers,
            endpoint& source,
            int flags) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
            , source_(source)
            , flags_(flags)
        {
        }

        std::coroutine_handle<>
        dispatch(std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return self_.get_impl().recv_from(
                h, ex, buffers_, &source_, flags_, this->token_, &this->ec_,
                &this->bytes_);
        }
    };

    struct native_wait_awaitable : detail::void_op_base<native_wait_awaitable>
    {
        native_udp_socket& self_;
        wait_type w_;

        native_wait_awaitable(native_udp_socket& self, wait_type w) noexcept
            : self_(self)
            , w_(w)
        {
        }

        std::coroutine_handle<>
        dispatch(std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return self_.get_impl().wait(h, ex, w_, this->token_, &this->ec_);
        }
    };

    struct native_connect_awaitable
        : detail::void_op_base<native_connect_awaitable>
    {
        native_udp_socket& self_;
        endpoint endpoint_;

        native_connect_awaitable(native_udp_socket& self, endpoint ep) noexcept
            : self_(self)
            , endpoint_(ep)
        {
        }

        std::coroutine_handle<>
        dispatch(std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return self_.get_impl().connect(
                h, ex, endpoint_, this->token_, &this->ec_);
        }
    };

    template<class ConstBufferSequence>
    struct native_send_awaitable
        : detail::bytes_op_base<native_send_awaitable<ConstBufferSequence>>
    {
        native_udp_socket& self_;
        ConstBufferSequence buffers_;
        int flags_;

        native_send_awaitable(
            native_udp_socket& self,
            ConstBufferSequence buffers,
            int flags) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
            , flags_(flags)
        {
        }

        std::coroutine_handle<>
        dispatch(std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return self_.get_impl().send(
                h, ex, buffers_, flags_, this->token_, &this->ec_,
                &this->bytes_);
        }
    };

    template<class MutableBufferSequence>
    struct native_recv_awaitable
        : detail::bytes_op_base<native_recv_awaitable<MutableBufferSequence>>
    {
        native_udp_socket& self_;
        MutableBufferSequence buffers_;
        int flags_;

        native_recv_awaitable(
            native_udp_socket& self,
            MutableBufferSequence buffers,
            int flags) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
            , flags_(flags)
        {
        }

        std::coroutine_handle<>
        dispatch(std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return self_.get_impl().recv(
                h, ex, buffers_, flags_, this->token_, &this->ec_,
                &this->bytes_);
        }
    };

public:
    /** Construct a native UDP socket from an execution context.

        @param ctx The execution context that owns this socket.
    */
    explicit native_udp_socket(capy::execution_context& ctx)
        : udp_socket(create_handle<service_type>(ctx))
    {
    }

    /** Construct a native UDP socket from an executor.

        @param ex The executor whose context owns the socket.

        @tparam Ex A type satisfying @ref capy::Executor. Must not
            be `native_udp_socket` itself (disables implicit
            conversion from move).
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, native_udp_socket>) &&
        capy::Executor<Ex>
    explicit native_udp_socket(Ex const& ex) : native_udp_socket(ex.context())
    {
    }

    /** Move construct.

        @param other The socket to move from.

        @pre No awaitables returned by @p other's methods exist.
        @pre The execution context associated with @p other must
            outlive this socket.
    */
    native_udp_socket(native_udp_socket&&) noexcept = default;

    /** Move assign.

        @param other The socket to move from.

        @pre No awaitables returned by either `*this` or @p other's
            methods exist.
        @pre The execution context associated with @p other must
            outlive this socket.
    */
    native_udp_socket& operator=(native_udp_socket&&) noexcept = default;

    /// Copy construction is disabled; the handle is uniquely owned.
    native_udp_socket(native_udp_socket const&) = delete;
    /// Copy assignment is disabled; the handle is uniquely owned.
    native_udp_socket& operator=(native_udp_socket const&) = delete;

    /** Send a datagram to the specified destination.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref udp_socket::send_to.

        @param buffers The buffer sequence containing data to send.
        @param dest The destination endpoint.
        @param flags Message flags.

        @return An awaitable yielding `(error_code, std::size_t)`.

        A closed socket reports `errc::bad_file_descriptor`.
    */
    template<capy::ConstBufferSequence CB>
    [[nodiscard]] auto
    send_to(CB const& buffers, endpoint dest, corosio::message_flags flags)
    {
        native_send_to_awaitable<CB> aw(
            *this, buffers, dest, static_cast<int>(flags));
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /// @overload
    template<capy::ConstBufferSequence CB>
    [[nodiscard]] auto send_to(CB const& buffers, endpoint dest)
    {
        return send_to(buffers, dest, corosio::message_flags::none);
    }

    /** Receive a datagram and capture the sender's endpoint.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref udp_socket::recv_from.

        @param buffers The buffer sequence to receive data into.
        @param source Reference to an endpoint that receives
            the sender's address on successful completion.
        @param flags Message flags (e.g. message_flags::peek).

        @return An awaitable yielding `(error_code, std::size_t)`.

        A closed socket reports `errc::bad_file_descriptor`.
    */
    template<capy::MutableBufferSequence MB>
    [[nodiscard]] auto
    recv_from(MB const& buffers, endpoint& source, corosio::message_flags flags)
    {
        native_recv_from_awaitable<MB> aw(
            *this, buffers, source, static_cast<int>(flags));
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /// @overload
    template<capy::MutableBufferSequence MB>
    [[nodiscard]] auto recv_from(MB const& buffers, endpoint& source)
    {
        return recv_from(buffers, source, corosio::message_flags::none);
    }

    /** Asynchronously connect to set the default peer.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref udp_socket::connect.

        If the socket is not already open, it is opened automatically
        using the address family of @p ep.

        @param ep The remote endpoint to connect to.

        @return An awaitable yielding `io_result<>`.

        If the socket needs to be opened and the open fails, the
        awaitable completes immediately with that error.
    */
    [[nodiscard]] auto connect(endpoint ep)
    {
        native_connect_awaitable aw(*this, ep);
        if (!is_open())
            aw.ec_ = open(ep.address().family());
        return aw;
    }

    /** Send a datagram to the connected peer.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref udp_socket::send.

        @param buffers The buffer sequence containing data to send.
        @param flags Message flags.

        @return An awaitable yielding `(error_code, std::size_t)`.

        A closed socket reports `errc::bad_file_descriptor`.
    */
    template<capy::ConstBufferSequence CB>
    [[nodiscard]] auto send(CB const& buffers, corosio::message_flags flags)
    {
        native_send_awaitable<CB> aw(*this, buffers, static_cast<int>(flags));
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /// @overload
    template<capy::ConstBufferSequence CB>
    [[nodiscard]] auto send(CB const& buffers)
    {
        return send(buffers, corosio::message_flags::none);
    }

    /** Receive a datagram from the connected peer.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref udp_socket::recv.

        @param buffers The buffer sequence to receive data into.
        @param flags Message flags (e.g. message_flags::peek).

        @return An awaitable yielding `(error_code, std::size_t)`.

        A closed socket reports `errc::bad_file_descriptor`.
    */
    template<capy::MutableBufferSequence MB>
    [[nodiscard]] auto recv(MB const& buffers, corosio::message_flags flags)
    {
        native_recv_awaitable<MB> aw(*this, buffers, static_cast<int>(flags));
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /// @overload
    template<capy::MutableBufferSequence MB>
    [[nodiscard]] auto recv(MB const& buffers)
    {
        return recv(buffers, corosio::message_flags::none);
    }

    /** Asynchronously wait for the socket to be ready.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref udp_socket::wait.

        @param w The wait direction (read, write, or error).

        @return An awaitable yielding `io_result<>`.
    */
    [[nodiscard]] auto wait(wait_type w)
    {
        return native_wait_awaitable(*this, w);
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_NATIVE_NATIVE_UDP_SOCKET_HPP
