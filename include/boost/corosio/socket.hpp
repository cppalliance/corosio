//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_SOCKET_HPP
#define BOOST_COROSIO_SOCKET_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/io_stream.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/corosio/io_buffer_param.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/concept/executor.hpp>

#include <boost/system/error_code.hpp>

#include <concepts>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <stop_token>
#include <type_traits>

namespace boost {
namespace corosio {

#ifdef _WIN32
using native_handle_type = std::uintptr_t;  // SOCKET
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

    @par Example
    @code
    io_context ioc;
    socket s(ioc);
    s.open();

    // Using structured bindings
    auto [ec] = co_await s.connect(
        endpoint(urls::ipv4_address::loopback(), 8080));
    if (ec)
        co_return;

    char buf[1024];
    auto [read_ec, n] = co_await s.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));
    @endcode
*/
class BOOST_COROSIO_DECL socket : public io_stream
{
public:
    /** Different ways a socket may be shutdown. */
    enum shutdown_type
    {
        shutdown_receive,
        shutdown_send,
        shutdown_both
    };

    /** Options for SO_LINGER socket option. */
    struct linger_options
    {
        bool enabled = false;
        int timeout = 0;  // seconds
    };

    struct socket_impl : io_stream_impl
    {
        virtual void connect(
            std::coroutine_handle<>,
            capy::executor_ref,
            endpoint,
            std::stop_token,
            system::error_code*) = 0;

        virtual system::error_code shutdown(shutdown_type) noexcept = 0;

        virtual native_handle_type native_handle() const noexcept = 0;

        // Socket options
        virtual system::error_code set_no_delay(bool value) noexcept = 0;
        virtual bool no_delay(system::error_code& ec) const noexcept = 0;

        virtual system::error_code set_keep_alive(bool value) noexcept = 0;
        virtual bool keep_alive(system::error_code& ec) const noexcept = 0;

        virtual system::error_code set_receive_buffer_size(int size) noexcept = 0;
        virtual int receive_buffer_size(system::error_code& ec) const noexcept = 0;

        virtual system::error_code set_send_buffer_size(int size) noexcept = 0;
        virtual int send_buffer_size(system::error_code& ec) const noexcept = 0;

        virtual system::error_code set_linger(bool enabled, int timeout) noexcept = 0;
        virtual linger_options linger(system::error_code& ec) const noexcept = 0;
    };

    struct connect_awaitable
    {
        socket& s_;
        endpoint endpoint_;
        std::stop_token token_;
        mutable system::error_code ec_;

        connect_awaitable(socket& s, endpoint ep) noexcept
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
                return {make_error_code(system::errc::operation_canceled)};
            return {ec_};
        }

        template<typename Ex>
        auto await_suspend(
            std::coroutine_handle<> h,
            Ex const& ex) -> std::coroutine_handle<>
        {
            s_.get().connect(h, ex, endpoint_, token_, &ec_);
            return std::noop_coroutine();
        }

        template<typename Ex>
        auto await_suspend(
            std::coroutine_handle<> h,
            Ex const& ex,
            std::stop_token token) -> std::coroutine_handle<>
        {
            token_ = std::move(token);
            s_.get().connect(h, ex, endpoint_, token_, &ec_);
            return std::noop_coroutine();
        }
    };

public:
    /** Destructor.

        Closes the socket if open, cancelling any pending operations.
    */
    ~socket();

    /** Construct a socket from an execution context.

        @param ctx The execution context that will own this socket.
    */
    explicit socket(capy::execution_context& ctx);

    /** Construct a socket from an executor.

        The socket is associated with the executor's context.

        @param ex The executor whose context will own the socket.
    */
    template<class Ex>
        requires (!std::same_as<std::remove_cvref_t<Ex>, socket>) &&
                 capy::Executor<Ex>
    explicit socket(Ex const& ex)
        : socket(ex.context())
    {
    }

    /** Move constructor.

        Transfers ownership of the socket resources.

        @param other The socket to move from.
    */
    socket(socket&& other) noexcept
        : io_stream(other.context())
    {
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }

    /** Move assignment operator.

        Closes any existing socket and transfers ownership.
        The source and destination must share the same execution context.

        @param other The socket to move from.

        @return Reference to this socket.

        @throws std::logic_error if the sockets have different execution contexts.
    */
    socket& operator=(socket&& other)
    {
        if (this != &other)
        {
            if (ctx_ != other.ctx_)
                detail::throw_logic_error(
                    "cannot move socket across execution contexts");
            close();
            impl_ = other.impl_;
            other.impl_ = nullptr;
        }
        return *this;
    }

    socket(socket const&) = delete;
    socket& operator=(socket const&) = delete;

    /** Open the socket.

        Creates an IPv4 TCP socket and associates it with the platform
        reactor (IOCP on Windows). This must be called before initiating
        I/O operations.

        @throws std::system_error on failure.
    */
    void open();

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
        return impl_ != nullptr;
    }

    /** Initiate an asynchronous connect operation.

        Connects the socket to the specified remote endpoint. The socket
        must be open before calling this function.

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

        @throws std::logic_error if the socket is not open.

        @par Preconditions
        The socket must be open (`is_open() == true`).

        @par Example
        @code
        auto [ec] = co_await s.connect(endpoint);
        if (ec) { ... }
        @endcode
    */
    auto connect(endpoint ep)
    {
        if (!impl_)
            detail::throw_logic_error("connect: socket not open");
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

    //--------------------------------------------------------------------------
    //
    // Socket Options
    //
    //--------------------------------------------------------------------------

    /** Enable or disable TCP_NODELAY (disable Nagle's algorithm).

        When enabled, segments are sent as soon as possible even if
        there is only a small amount of data. This reduces latency
        at the potential cost of increased network traffic.

        @param value `true` to disable Nagle's algorithm (enable no-delay).

        @throws std::logic_error if the socket is not open.
        @throws std::system_error on failure.
    */
    void set_no_delay(bool value);

    /** Get the current TCP_NODELAY setting.

        @return `true` if Nagle's algorithm is disabled.

        @throws std::logic_error if the socket is not open.
        @throws std::system_error on failure.
    */
    bool no_delay() const;

    /** Enable or disable SO_KEEPALIVE.

        When enabled, the socket will periodically send keepalive probes
        to detect if the peer is still reachable.

        @param value `true` to enable keepalive probes.

        @throws std::logic_error if the socket is not open.
        @throws std::system_error on failure.
    */
    void set_keep_alive(bool value);

    /** Get the current SO_KEEPALIVE setting.

        @return `true` if keepalive is enabled.

        @throws std::logic_error if the socket is not open.
        @throws std::system_error on failure.
    */
    bool keep_alive() const;

    /** Set the receive buffer size (SO_RCVBUF).

        @param size The desired receive buffer size in bytes.

        @throws std::logic_error if the socket is not open.
        @throws std::system_error on failure.

        @note The operating system may adjust the actual buffer size.
    */
    void set_receive_buffer_size(int size);

    /** Get the receive buffer size (SO_RCVBUF).

        @return The current receive buffer size in bytes.

        @throws std::logic_error if the socket is not open.
        @throws std::system_error on failure.
    */
    int receive_buffer_size() const;

    /** Set the send buffer size (SO_SNDBUF).

        @param size The desired send buffer size in bytes.

        @throws std::logic_error if the socket is not open.
        @throws std::system_error on failure.

        @note The operating system may adjust the actual buffer size.
    */
    void set_send_buffer_size(int size);

    /** Get the send buffer size (SO_SNDBUF).

        @return The current send buffer size in bytes.

        @throws std::logic_error if the socket is not open.
        @throws std::system_error on failure.
    */
    int send_buffer_size() const;

    /** Set the SO_LINGER option.

        Controls behavior when closing a socket with unsent data.

        @param enabled If `true`, close() will block until data is sent
            or the timeout expires. If `false`, close() returns immediately.
        @param timeout The linger timeout in seconds (only used if enabled).

        @throws std::logic_error if the socket is not open.
        @throws std::system_error on failure.
    */
    void set_linger(bool enabled, int timeout);

    /** Get the current SO_LINGER setting.

        @return The current linger options.

        @throws std::logic_error if the socket is not open.
        @throws std::system_error on failure.
    */
    linger_options linger() const;

private:
    friend class acceptor;

    inline socket_impl& get() const noexcept
    {
        return *static_cast<socket_impl*>(impl_);
    }
};

} // namespace corosio
} // namespace boost

#endif
