//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TCP_SOCKET_HPP
#define BOOST_COROSIO_TCP_SOCKET_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/io/io_stream.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/corosio/detail/buffer_param.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/tcp.hpp>
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

/// Represent a platform-specific socket descriptor (`int` on POSIX, `SOCKET` on Windows).
#if BOOST_COROSIO_HAS_IOCP && !defined(BOOST_COROSIO_MRDOCS)
using native_handle_type = std::uintptr_t;
#else
using native_handle_type = int;
#endif

/** An asynchronous TCP socket for coroutine I/O.

    This class provides asynchronous TCP socket operations that return
    awaitable types. Each operation participates in the affine awaitable
    protocol, ensuring coroutines resume on the correct executor.

    The socket must be opened before performing I/O operations. Operations
    support cancellation through `std::stop_token` via the affine protocol,
    or explicitly through the `cancel()` member function.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. A socket must not have concurrent operations
    of the same type (e.g., two simultaneous reads). One read and one
    write may be in flight simultaneously.

    @par Semantics
    Wraps the platform TCP/IP stack. Operations dispatch to
    OS socket APIs via the io_context reactor (epoll, IOCP,
    kqueue). Satisfies @ref capy::Stream.

    @par Example
    @code
    io_context ioc;
    tcp_socket s(ioc);
    s.open();

    // Using structured bindings
    auto [ec] = co_await s.connect(
        endpoint(ipv4_address::loopback(), 8080));
    if (ec)
        co_return;

    char buf[1024];
    auto [read_ec, n] = co_await s.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));
    @endcode
*/
class BOOST_COROSIO_DECL tcp_socket : public io_stream
{
public:
    /** Different ways a socket may be shutdown. */
    enum shutdown_type
    {
        shutdown_receive,
        shutdown_send,
        shutdown_both
    };

    /** Define backend hooks for TCP socket operations.

        Platform backends (epoll, IOCP, kqueue, select) derive from
        this to implement socket I/O, connection, and option management.
    */
    struct implementation : io_stream::implementation
    {
        /** Initiate an asynchronous connect to the given endpoint.

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

        /** Shut down the socket for the given direction(s).

            @param what The shutdown direction.

            @return Error code on failure, empty on success.
        */
        virtual std::error_code shutdown(shutdown_type what) noexcept = 0;

        /// Return the platform socket descriptor.
        virtual native_handle_type native_handle() const noexcept = 0;

        /** Request cancellation of pending asynchronous operations.

            All outstanding operations complete with operation_canceled error.
            Check `ec == cond::canceled` for portable comparison.
        */
        virtual void cancel() noexcept = 0;

        /** Set a socket option.

            @param level The protocol level (e.g. `SOL_SOCKET`).
            @param optname The option name (e.g. `SO_KEEPALIVE`).
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
            @param optname The option name (e.g. `SO_KEEPALIVE`).
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

        /// Return the cached remote endpoint.
        virtual endpoint remote_endpoint() const noexcept = 0;
    };

    /// Represent the awaitable returned by @ref connect.
    struct connect_awaitable
    {
        tcp_socket& s_;
        endpoint endpoint_;
        std::stop_token token_;
        mutable std::error_code ec_;

        connect_awaitable(tcp_socket& s, endpoint ep) noexcept
            : s_(s)
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
            return s_.get().connect(h, env->executor, endpoint_, token_, &ec_);
        }
    };

public:
    /** Destructor.

        Closes the socket if open, cancelling any pending operations.
    */
    ~tcp_socket() override;

    /** Construct a socket from an execution context.

        @param ctx The execution context that will own this socket.
    */
    explicit tcp_socket(capy::execution_context& ctx);

    /** Construct a socket from an executor.

        The socket is associated with the executor's context.

        @param ex The executor whose context will own the socket.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, tcp_socket>) &&
        capy::Executor<Ex>
    explicit tcp_socket(Ex const& ex) : tcp_socket(ex.context())
    {
    }

    /** Move constructor.

        Transfers ownership of the socket resources.

        @param other The socket to move from.

        @pre No awaitables returned by @p other's methods exist.
        @pre @p other is not referenced as a peer in any outstanding
            accept awaitable.
        @pre The execution context associated with @p other must
            outlive this socket.
    */
    tcp_socket(tcp_socket&& other) noexcept : io_object(std::move(other)) {}

    /** Move assignment operator.

        Closes any existing socket and transfers ownership.

        @param other The socket to move from.

        @pre No awaitables returned by either `*this` or @p other's
            methods exist.
        @pre Neither `*this` nor @p other is referenced as a peer in
            any outstanding accept awaitable.
        @pre The execution context associated with @p other must
            outlive this socket.

        @return Reference to this socket.
    */
    tcp_socket& operator=(tcp_socket&& other) noexcept
    {
        if (this != &other)
        {
            close();
            h_ = std::move(other.h_);
        }
        return *this;
    }

    tcp_socket(tcp_socket const&)            = delete;
    tcp_socket& operator=(tcp_socket const&) = delete;

    /** Open the socket.

        Creates a TCP socket and associates it with the platform
        reactor (IOCP on Windows). Calling @ref connect on a closed
        socket opens it automatically with the endpoint's address family,
        so explicit `open()` is only needed when socket options must be
        set before connecting.

        @param proto The protocol (IPv4 or IPv6). Defaults to
            `tcp::v4()`.

        @throws std::system_error on failure.
    */
    void open(tcp proto = tcp::v4());

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

    /** Initiate an asynchronous connect operation.

        If the socket is not already open, it is opened automatically
        using the address family of @p ep (IPv4 or IPv6). If the socket
        is already open, the existing file descriptor is used as-is.

        The operation supports cancellation via `std::stop_token` through
        the affine awaitable protocol. If the associated stop token is
        triggered, the operation completes immediately with
        `errc::operation_canceled`.

        @param ep The remote endpoint to connect to.

        @return An awaitable that completes with `io_result<>`.
            Returns success (default error_code) on successful connection,
            or an error code on failure including:
            - connection_refused: No server listening at endpoint
            - timed_out: Connection attempt timed out
            - network_unreachable: No route to host
            - operation_canceled: Cancelled via stop_token or cancel().
                Check `ec == cond::canceled` for portable comparison.

        @throws std::system_error if the socket needs to be opened
            and the open fails.

        @par Preconditions
        This socket must outlive the returned awaitable.

        @par Example
        @code
        // Socket opened automatically with correct address family:
        auto [ec] = co_await s.connect(endpoint);
        if (ec) { ... }
        @endcode
    */
    auto connect(endpoint ep)
    {
        if (!is_open())
            open(ep.is_v6() ? tcp::v6() : tcp::v4());
        return connect_awaitable(*this, ep);
    }

    /** Cancel any pending asynchronous operations.

        All outstanding operations complete with `errc::operation_canceled`.
        Check `ec == cond::canceled` for portable comparison.
    */
    void cancel();

    /** Get the native socket handle.

        Returns the underlying platform-specific socket descriptor.
        On POSIX systems this is an `int` file descriptor.
        On Windows this is a `SOCKET` handle.

        @return The native socket handle, or -1/INVALID_SOCKET if not open.

        @par Preconditions
        None. May be called on closed sockets.
    */
    native_handle_type native_handle() const noexcept;

    /** Disable sends or receives on the socket.

        TCP connections are full-duplex: each direction (send and receive)
        operates independently. This function allows you to close one or
        both directions without destroying the socket.

        @li @ref shutdown_send sends a TCP FIN packet to the peer,
            signaling that you have no more data to send. You can still
            receive data until the peer also closes their send direction.
            This is the most common use case, typically called before
            close() to ensure graceful connection termination.

        @li @ref shutdown_receive disables reading on the socket. This
            does NOT send anything to the peer - they are not informed
            and may continue sending data. Subsequent reads will fail
            or return end-of-file. Incoming data may be discarded or
            buffered depending on the operating system.

        @li @ref shutdown_both combines both effects: sends a FIN and
            disables reading.

        When the peer shuts down their send direction (sends a FIN),
        subsequent read operations will complete with `capy::cond::eof`.
        Use the portable condition test rather than comparing error
        codes directly:

        @code
        auto [ec, n] = co_await sock.read_some(buffer);
        if (ec == capy::cond::eof)
        {
            // Peer closed their send direction
        }
        @endcode

        Any error from the underlying system call is silently discarded
        because it is unlikely to be helpful.

        @param what Determines what operations will no longer be allowed.
    */
    void shutdown(shutdown_type what);

    /** Set a socket option.

        Applies a type-safe socket option to the underlying socket.
        The option type encodes the protocol level and option name.

        @par Example
        @code
        sock.set_option( socket_option::no_delay( true ) );
        sock.set_option( socket_option::receive_buffer_size( 65536 ) );
        @endcode

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
            detail::throw_system_error(ec, "tcp_socket::set_option");
    }

    /** Get a socket option.

        Retrieves the current value of a type-safe socket option.

        @par Example
        @code
        auto nd = sock.get_option<socket_option::no_delay>();
        if ( nd.value() )
            // Nagle's algorithm is disabled
        @endcode

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
            detail::throw_system_error(ec, "tcp_socket::get_option");
        opt.resize(sz);
        return opt;
    }

    /** Get the local endpoint of the socket.

        Returns the local address and port to which the socket is bound.
        For a connected socket, this is the local side of the connection.
        The endpoint is cached when the connection is established.

        @return The local endpoint, or a default endpoint (0.0.0.0:0) if
            the socket is not connected.

        @par Thread Safety
        The cached endpoint value is set during connect/accept completion
        and cleared during close(). This function may be called concurrently
        with I/O operations, but must not be called concurrently with
        connect(), accept(), or close().
    */
    endpoint local_endpoint() const noexcept;

    /** Get the remote endpoint of the socket.

        Returns the remote address and port to which the socket is connected.
        The endpoint is cached when the connection is established.

        @return The remote endpoint, or a default endpoint (0.0.0.0:0) if
            the socket is not connected.

        @par Thread Safety
        The cached endpoint value is set during connect/accept completion
        and cleared during close(). This function may be called concurrently
        with I/O operations, but must not be called concurrently with
        connect(), accept(), or close().
    */
    endpoint remote_endpoint() const noexcept;

protected:
    tcp_socket() noexcept = default;

    explicit tcp_socket(handle h) noexcept : io_object(std::move(h)) {}

private:
    friend class tcp_acceptor;

    /// Open the socket for the given protocol triple.
    void open_for_family(int family, int type, int protocol);

    inline implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif
