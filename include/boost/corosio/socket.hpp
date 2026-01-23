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
#include <boost/capy/io_awaitable.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/concept/executor.hpp>

#include <boost/system/error_code.hpp>

#include <cassert>
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

    // Or using exceptions
    (co_await s.connect(endpoint)).value();
    auto bytes = (co_await s.read_some(buf)).value();
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

        @par Preconditions
        The socket must be open (`is_open() == true`).

        @par Example
        @code
        // Using structured bindings
        auto [ec] = co_await s.connect(endpoint);
        if (ec) { ... }

        // Using exceptions
        (co_await s.connect(endpoint)).value();
        @endcode
    */
    auto connect(endpoint ep)
    {
        assert(impl_ != nullptr);
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
