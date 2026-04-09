//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_UDP_SOCKET_HPP
#define BOOST_COROSIO_UDP_SOCKET_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/detail/op_base.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/corosio/detail/buffer_param.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/message_flags.hpp>
#include <boost/corosio/udp.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/concept/executor.hpp>

#include <system_error>

#include <concepts>
#include <coroutine>
#include <cstddef>
#include <stop_token>
#include <type_traits>

namespace boost::corosio {

/** An asynchronous UDP socket for coroutine I/O.

    This class provides asynchronous UDP datagram operations that
    return awaitable types. Each operation participates in the affine
    awaitable protocol, ensuring coroutines resume on the correct
    executor.

    Supports two modes of operation:

    **Connectionless mode**: each `send_to` specifies a destination
    endpoint, and each `recv_from` captures the source endpoint.
    The socket must be opened (and optionally bound) before I/O.

    **Connected mode**: call `connect()` to set a default peer,
    then use `send()`/`recv()` without endpoint arguments.
    The kernel filters incoming datagrams to those from the
    connected peer.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. A socket must not have concurrent
    operations of the same type (e.g., two simultaneous recv_from).
    One send_to and one recv_from may be in flight simultaneously.

    @par Example
    @code
    // Connectionless mode
    io_context ioc;
    udp_socket sock( ioc );
    sock.open( udp::v4() );
    sock.bind( endpoint( ipv4_address::any(), 9000 ) );

    char buf[1024];
    endpoint sender;
    auto [ec, n] = co_await sock.recv_from(
        capy::mutable_buffer( buf, sizeof( buf ) ), sender );
    if ( !ec )
        co_await sock.send_to(
            capy::const_buffer( buf, n ), sender );

    // Connected mode
    udp_socket csock( ioc );
    auto [cec] = co_await csock.connect(
        endpoint( ipv4_address::loopback(), 9000 ) );
    if ( !cec )
        co_await csock.send(
            capy::const_buffer( buf, n ) );
    @endcode
*/
class BOOST_COROSIO_DECL udp_socket : public io_object
{
public:
    /** Define backend hooks for UDP socket operations.

        Platform backends (epoll, kqueue, select) derive from
        this to implement datagram I/O and option management.
    */
    struct implementation : io_object::implementation
    {
        /** Initiate an asynchronous send_to operation.

            @param h Coroutine handle to resume on completion.
            @param ex Executor for dispatching the completion.
            @param buf The buffer data to send.
            @param dest The destination endpoint.
            @param token Stop token for cancellation.
            @param ec Output error code.
            @param bytes_out Output bytes transferred.

            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> send_to(
            std::coroutine_handle<> h,
            capy::executor_ref ex,
            buffer_param buf,
            endpoint dest,
            int flags,
            std::stop_token token,
            std::error_code* ec,
            std::size_t* bytes_out) = 0;

        /** Initiate an asynchronous recv_from operation.

            @param h Coroutine handle to resume on completion.
            @param ex Executor for dispatching the completion.
            @param buf The buffer to receive into.
            @param source Output endpoint for the sender's address.
            @param token Stop token for cancellation.
            @param ec Output error code.
            @param bytes_out Output bytes transferred.

            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> recv_from(
            std::coroutine_handle<> h,
            capy::executor_ref ex,
            buffer_param buf,
            endpoint* source,
            int flags,
            std::stop_token token,
            std::error_code* ec,
            std::size_t* bytes_out) = 0;

        /// Return the platform socket descriptor.
        virtual native_handle_type native_handle() const noexcept = 0;

        /** Request cancellation of pending asynchronous operations.

            All outstanding operations complete with operation_canceled
            error. Check `ec == cond::canceled` for portable comparison.
        */
        virtual void cancel() noexcept = 0;

        /** Set a socket option.

            @param level The protocol level (e.g. `SOL_SOCKET`).
            @param optname The option name.
            @param data Pointer to the option value.
            @param size Size of the option value in bytes.
            @return Error code on failure, empty on success.
        */
        virtual std::error_code set_option(
            int level,
            int optname,
            void const* data,
            std::size_t size) noexcept = 0;

        /** Get a socket option.

            @param level The protocol level (e.g. `SOL_SOCKET`).
            @param optname The option name.
            @param data Pointer to receive the option value.
            @param size On entry, the size of the buffer. On exit,
                the size of the option value.
            @return Error code on failure, empty on success.
        */
        virtual std::error_code
        get_option(int level, int optname, void* data, std::size_t* size)
            const noexcept = 0;

        /// Return the cached local endpoint.
        virtual endpoint local_endpoint() const noexcept = 0;

        /// Return the cached remote endpoint (connected mode).
        virtual endpoint remote_endpoint() const noexcept = 0;

        /** Initiate an asynchronous connect to set the default peer.

            @param h Coroutine handle to resume on completion.
            @param ex Executor for dispatching the completion.
            @param ep The remote endpoint to connect to.
            @param token Stop token for cancellation.
            @param ec Output error code.

            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> connect(
            std::coroutine_handle<> h,
            capy::executor_ref ex,
            endpoint ep,
            std::stop_token token,
            std::error_code* ec) = 0;

        /** Initiate an asynchronous connected send operation.

            @param h Coroutine handle to resume on completion.
            @param ex Executor for dispatching the completion.
            @param buf The buffer data to send.
            @param token Stop token for cancellation.
            @param ec Output error code.
            @param bytes_out Output bytes transferred.

            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> send(
            std::coroutine_handle<> h,
            capy::executor_ref ex,
            buffer_param buf,
            int flags,
            std::stop_token token,
            std::error_code* ec,
            std::size_t* bytes_out) = 0;

        /** Initiate an asynchronous connected recv operation.

            @param h Coroutine handle to resume on completion.
            @param ex Executor for dispatching the completion.
            @param buf The buffer to receive into.
            @param token Stop token for cancellation.
            @param ec Output error code.
            @param bytes_out Output bytes transferred.

            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> recv(
            std::coroutine_handle<> h,
            capy::executor_ref ex,
            buffer_param buf,
            int flags,
            std::stop_token token,
            std::error_code* ec,
            std::size_t* bytes_out) = 0;
    };

    /** Represent the awaitable returned by @ref send_to.

        Captures the destination endpoint and buffer, then dispatches
        to the backend implementation on suspension.
    */
    struct send_to_awaitable
        : detail::bytes_op_base<send_to_awaitable>
    {
        udp_socket& s_;
        buffer_param buf_;
        endpoint dest_;
        int flags_;

        send_to_awaitable(
            udp_socket& s, buffer_param buf,
            endpoint dest, int flags = 0) noexcept
            : s_(s), buf_(buf), dest_(dest), flags_(flags) {}

        std::coroutine_handle<> dispatch(
            std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return s_.get().send_to(
                h, ex, buf_, dest_, flags_, token_, &ec_, &bytes_);
        }
    };

    struct recv_from_awaitable
        : detail::bytes_op_base<recv_from_awaitable>
    {
        udp_socket& s_;
        buffer_param buf_;
        endpoint& source_;
        int flags_;

        recv_from_awaitable(
            udp_socket& s, buffer_param buf,
            endpoint& source, int flags = 0) noexcept
            : s_(s), buf_(buf), source_(source), flags_(flags) {}

        std::coroutine_handle<> dispatch(
            std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return s_.get().recv_from(
                h, ex, buf_, &source_, flags_, token_, &ec_, &bytes_);
        }
    };

    struct connect_awaitable
        : detail::void_op_base<connect_awaitable>
    {
        udp_socket& s_;
        endpoint endpoint_;

        connect_awaitable(udp_socket& s, endpoint ep) noexcept
            : s_(s), endpoint_(ep) {}

        std::coroutine_handle<> dispatch(
            std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return s_.get().connect(h, ex, endpoint_, token_, &ec_);
        }
    };

    struct send_awaitable
        : detail::bytes_op_base<send_awaitable>
    {
        udp_socket& s_;
        buffer_param buf_;
        int flags_;

        send_awaitable(
            udp_socket& s, buffer_param buf,
            int flags = 0) noexcept
            : s_(s), buf_(buf), flags_(flags) {}

        std::coroutine_handle<> dispatch(
            std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return s_.get().send(
                h, ex, buf_, flags_, token_, &ec_, &bytes_);
        }
    };

    struct recv_awaitable
        : detail::bytes_op_base<recv_awaitable>
    {
        udp_socket& s_;
        buffer_param buf_;
        int flags_;

        recv_awaitable(
            udp_socket& s, buffer_param buf,
            int flags = 0) noexcept
            : s_(s), buf_(buf), flags_(flags) {}

        std::coroutine_handle<> dispatch(
            std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return s_.get().recv(
                h, ex, buf_, flags_, token_, &ec_, &bytes_);
        }
    };

public:
    /** Destructor.

        Closes the socket if open, cancelling any pending operations.
    */
    ~udp_socket() override;

    /** Construct a socket from an execution context.

        @param ctx The execution context that will own this socket.
    */
    explicit udp_socket(capy::execution_context& ctx);

    /** Construct a socket from an executor.

        The socket is associated with the executor's context.

        @param ex The executor whose context will own the socket.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, udp_socket>) &&
        capy::Executor<Ex>
    explicit udp_socket(Ex const& ex) : udp_socket(ex.context())
    {
    }

    /** Move constructor.

        Transfers ownership of the socket resources.

        @param other The socket to move from.
    */
    udp_socket(udp_socket&& other) noexcept : io_object(std::move(other)) {}

    /** Move assignment operator.

        Closes any existing socket and transfers ownership.

        @param other The socket to move from.
        @return Reference to this socket.
    */
    udp_socket& operator=(udp_socket&& other) noexcept
    {
        if (this != &other)
        {
            close();
            h_ = std::move(other.h_);
        }
        return *this;
    }

    udp_socket(udp_socket const&)            = delete;
    udp_socket& operator=(udp_socket const&) = delete;

    /** Open the socket.

        Creates a UDP socket and associates it with the platform
        reactor.

        @param proto The protocol (IPv4 or IPv6). Defaults to
            `udp::v4()`.

        @throws std::system_error on failure.
    */
    void open(udp proto = udp::v4());

    /** Close the socket.

        Releases socket resources. Any pending operations complete
        with `errc::operation_canceled`.
    */
    void close();

    /** Check if the socket is open.

        @return `true` if the socket is open and ready for operations.
    */
    bool is_open() const noexcept
    {
#if BOOST_COROSIO_HAS_IOCP && !defined(BOOST_COROSIO_MRDOCS)
        return h_ && get().native_handle() != ~native_handle_type(0);
#else
        return h_ && get().native_handle() >= 0;
#endif
    }

    /** Bind the socket to a local endpoint.

        Associates the socket with a local address and port.
        Required before calling `recv_from`.

        @param ep The local endpoint to bind to.

        @return Error code on failure, empty on success.

        @throws std::logic_error if the socket is not open.
    */
    [[nodiscard]] std::error_code bind(endpoint ep);

    /** Cancel any pending asynchronous operations.

        All outstanding operations complete with
        `errc::operation_canceled`. Check `ec == cond::canceled`
        for portable comparison.
    */
    void cancel();

    /** Get the native socket handle.

        @return The native socket handle, or -1 if not open.
    */
    native_handle_type native_handle() const noexcept;

    /** Set a socket option.

        @param opt The option to set.

        @throws std::logic_error if the socket is not open.
        @throws std::system_error on failure.
    */
    template<class Option>
    void set_option(Option const& opt)
    {
        if (!is_open())
            detail::throw_logic_error("set_option: socket not open");
        std::error_code ec = get().set_option(
            Option::level(), Option::name(), opt.data(), opt.size());
        if (ec)
            detail::throw_system_error(ec, "udp_socket::set_option");
    }

    /** Get a socket option.

        @return The current option value.

        @throws std::logic_error if the socket is not open.
        @throws std::system_error on failure.
    */
    template<class Option>
    Option get_option() const
    {
        if (!is_open())
            detail::throw_logic_error("get_option: socket not open");
        Option opt{};
        std::size_t sz = opt.size();
        std::error_code ec =
            get().get_option(Option::level(), Option::name(), opt.data(), &sz);
        if (ec)
            detail::throw_system_error(ec, "udp_socket::get_option");
        opt.resize(sz);
        return opt;
    }

    /** Get the local endpoint of the socket.

        @return The local endpoint, or a default endpoint if not bound.
    */
    endpoint local_endpoint() const noexcept;

    /** Send a datagram to the specified destination.

        @param buf The buffer containing data to send.
        @param dest The destination endpoint.
        @param flags Message flags (e.g. message_flags::dont_route).

        @par Cancellation
        Supports cancellation via the awaitable's stop_token or by
        calling @ref cancel. On cancellation, yields
        `errc::operation_canceled`.

        @return An awaitable that completes with
            `io_result<std::size_t>`.

        @throws std::logic_error if the socket is not open.
    */
    template<capy::ConstBufferSequence Buffers>
    auto send_to(
        Buffers const& buf,
        endpoint dest,
        corosio::message_flags flags)
    {
        if (!is_open())
            detail::throw_logic_error("send_to: socket not open");
        return send_to_awaitable(
            *this, buf, dest, static_cast<int>(flags));
    }

    /// @overload
    template<capy::ConstBufferSequence Buffers>
    auto send_to(Buffers const& buf, endpoint dest)
    {
        return send_to(buf, dest, corosio::message_flags::none);
    }

    /** Receive a datagram and capture the sender's endpoint.

        @param buf The buffer to receive data into.
        @param source Reference to an endpoint that will be set to
            the sender's address on successful completion.
        @param flags Message flags (e.g. message_flags::peek).

        @par Cancellation
        Supports cancellation via the awaitable's stop_token or by
        calling @ref cancel. On cancellation, yields
        `errc::operation_canceled`.

        @return An awaitable that completes with
            `io_result<std::size_t>`.

        @throws std::logic_error if the socket is not open.
    */
    template<capy::MutableBufferSequence Buffers>
    auto recv_from(
        Buffers const& buf,
        endpoint& source,
        corosio::message_flags flags)
    {
        if (!is_open())
            detail::throw_logic_error("recv_from: socket not open");
        return recv_from_awaitable(
            *this, buf, source, static_cast<int>(flags));
    }

    /// @overload
    template<capy::MutableBufferSequence Buffers>
    auto recv_from(Buffers const& buf, endpoint& source)
    {
        return recv_from(buf, source, corosio::message_flags::none);
    }

    /** Initiate an asynchronous connect to set the default peer.

        If the socket is not already open, it is opened automatically
        using the address family of @p ep.

        @param ep The remote endpoint to connect to.

        @par Cancellation
        Supports cancellation via the awaitable's stop_token or by
        calling @ref cancel. On cancellation, yields
        `errc::operation_canceled`.

        @return An awaitable that completes with `io_result<>`.

        @throws std::system_error if the socket needs to be opened
            and the open fails.
    */
    auto connect(endpoint ep)
    {
        if (!is_open())
            open(ep.is_v6() ? udp::v6() : udp::v4());
        return connect_awaitable(*this, ep);
    }

    /** Send a datagram to the connected peer.

        @param buf The buffer containing data to send.
        @param flags Message flags.

        @par Cancellation
        Supports cancellation via the awaitable's stop_token or by
        calling @ref cancel. On cancellation, yields
        `errc::operation_canceled`.

        @return An awaitable that completes with
            `io_result<std::size_t>`.

        @throws std::logic_error if the socket is not open.
    */
    template<capy::ConstBufferSequence Buffers>
    auto send(Buffers const& buf, corosio::message_flags flags)
    {
        if (!is_open())
            detail::throw_logic_error("send: socket not open");
        return send_awaitable(
            *this, buf, static_cast<int>(flags));
    }

    /// @overload
    template<capy::ConstBufferSequence Buffers>
    auto send(Buffers const& buf)
    {
        return send(buf, corosio::message_flags::none);
    }

    /** Receive a datagram from the connected peer.

        @param buf The buffer to receive data into.
        @param flags Message flags (e.g. message_flags::peek).

        @par Cancellation
        Supports cancellation via the awaitable's stop_token or by
        calling @ref cancel. On cancellation, yields
        `errc::operation_canceled`.

        @return An awaitable that completes with
            `io_result<std::size_t>`.

        @throws std::logic_error if the socket is not open.
    */
    template<capy::MutableBufferSequence Buffers>
    auto recv(Buffers const& buf, corosio::message_flags flags)
    {
        if (!is_open())
            detail::throw_logic_error("recv: socket not open");
        return recv_awaitable(
            *this, buf, static_cast<int>(flags));
    }

    /// @overload
    template<capy::MutableBufferSequence Buffers>
    auto recv(Buffers const& buf)
    {
        return recv(buf, corosio::message_flags::none);
    }

    /** Get the remote endpoint of the socket.

        Returns the address and port of the connected peer.

        @return The remote endpoint, or a default endpoint if
            not connected.
    */
    endpoint remote_endpoint() const noexcept;

protected:
    /// Construct from a pre-built handle (for native_udp_socket).
    explicit udp_socket(io_object::handle h) noexcept : io_object(std::move(h))
    {
    }

private:
    /// Open the socket for the given protocol triple.
    void open_for_family(int family, int type, int protocol);

    inline implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_UDP_SOCKET_HPP
