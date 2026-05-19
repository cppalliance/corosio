//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_LOCAL_STREAM_SOCKET_HPP
#define BOOST_COROSIO_LOCAL_STREAM_SOCKET_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/detail/op_base.hpp>
#include <boost/corosio/io/io_stream.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/corosio/detail/buffer_param.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream.hpp>
#include <boost/corosio/shutdown_type.hpp>
#include <boost/corosio/wait_type.hpp>
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

/** An asynchronous Unix stream socket for coroutine I/O.

    This class provides asynchronous Unix domain stream socket
    operations that return awaitable types. Each operation
    participates in the affine awaitable protocol, ensuring
    coroutines resume on the correct executor.

    The socket must be opened before performing I/O operations.
    Operations support cancellation through `std::stop_token` via
    the affine protocol, or explicitly through the `cancel()`
    member function.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. A socket must not have concurrent
    operations of the same type (e.g., two simultaneous reads).
    One read and one write may be in flight simultaneously.

    @par Semantics
    Wraps the platform Unix domain socket stack. Operations
    dispatch to OS socket APIs via the io_context backend
    (epoll, kqueue, select, or IOCP). Satisfies @ref capy::Stream.

    @par Example
    @code
    io_context ioc;
    local_stream_socket s(ioc);
    s.open();

    auto [ec] = co_await s.connect(local_endpoint("/tmp/my.sock"));
    if (ec)
        co_return;

    char buf[1024];
    auto [read_ec, n] = co_await s.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));
    @endcode
*/
class BOOST_COROSIO_DECL local_stream_socket : public io_stream
{
public:
    /// The endpoint type used by this socket.
    using endpoint_type = corosio::local_endpoint;

    using shutdown_type = corosio::shutdown_type;
    using enum corosio::shutdown_type;

    /** Define backend hooks for local stream socket operations.

        Platform backends (epoll, kqueue, select) derive from this
        to implement socket I/O, connection, and option management.
    */
    struct implementation : io_stream::implementation
    {
        /** Initiate an asynchronous connect to the given endpoint.

            @param h Coroutine handle to resume on completion.
            @param ex Executor for dispatching the completion.
            @param ep The local endpoint (path) to connect to.
            @param token Stop token for cancellation.
            @param ec Output error code.

            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> connect(
            std::coroutine_handle<> h,
            capy::executor_ref ex,
            corosio::local_endpoint ep,
            std::stop_token token,
            std::error_code* ec) = 0;

        /** Initiate an asynchronous wait for socket readiness.

            Completes when the socket becomes ready for the
            specified direction, or an error condition is
            reported. No bytes are transferred.

            @param h Coroutine handle to resume on completion.
            @param ex Executor for dispatching the completion.
            @param w The direction to wait on.
            @param token Stop token for cancellation.
            @param ec Output error code.

            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> wait(
            std::coroutine_handle<> h,
            capy::executor_ref ex,
            wait_type w,
            std::stop_token token,
            std::error_code* ec) = 0;

        /** Shut down the socket for the given direction(s).

            @param what The shutdown direction.

            @return Error code on failure, empty on success.
        */
        virtual std::error_code shutdown(shutdown_type what) noexcept = 0;

        /// Return the platform socket descriptor.
        virtual native_handle_type native_handle() const noexcept = 0;

        /** Release ownership of the native socket handle.

            Deregisters the socket from the reactor without closing
            the descriptor. The caller takes ownership.

            @return The native handle.
        */
        virtual native_handle_type release_socket() noexcept = 0;

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
        virtual corosio::local_endpoint local_endpoint() const noexcept = 0;

        /// Return the cached remote endpoint.
        virtual corosio::local_endpoint remote_endpoint() const noexcept = 0;
    };

    /// Represent the awaitable returned by @ref connect.
    struct connect_awaitable
        : detail::void_op_base<connect_awaitable>
    {
        local_stream_socket& s_;
        corosio::local_endpoint endpoint_;

        connect_awaitable(
            local_stream_socket& s, corosio::local_endpoint ep) noexcept
            : s_(s), endpoint_(ep) {}

        std::coroutine_handle<> dispatch(
            std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return s_.get().connect(h, ex, endpoint_, token_, &ec_);
        }
    };

    /// Represent the awaitable returned by @ref wait.
    struct wait_awaitable
        : detail::void_op_base<wait_awaitable>
    {
        local_stream_socket& s_;
        wait_type w_;

        wait_awaitable(local_stream_socket& s, wait_type w) noexcept
            : s_(s), w_(w) {}

        std::coroutine_handle<> dispatch(
            std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return s_.get().wait(h, ex, w_, token_, &ec_);
        }
    };

public:
    /** Destructor.

        Closes the socket if open, cancelling any pending operations.
    */
    ~local_stream_socket() override;

    /** Construct a socket from an execution context.

        @param ctx The execution context that will own this socket.
    */
    explicit local_stream_socket(capy::execution_context& ctx);

    /** Construct a socket from an executor.

        The socket is associated with the executor's context.

        @param ex The executor whose context will own the socket.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, local_stream_socket>) &&
        capy::Executor<Ex>
    explicit local_stream_socket(Ex const& ex) : local_stream_socket(ex.context())
    {
    }

    /** Move constructor.

        Transfers ownership of the socket resources.

        @param other The socket to move from.

        @pre No awaitables returned by @p other's methods exist.
        @pre The execution context associated with @p other must
            outlive this socket.
    */
    local_stream_socket(local_stream_socket&& other) noexcept
        : io_object(std::move(other))
    {
    }

    /** Move assignment operator.

        Closes any existing socket and transfers ownership.

        @param other The socket to move from.

        @pre No awaitables returned by either `*this` or @p other's
            methods exist.
        @pre The execution context associated with @p other must
            outlive this socket.

        @return Reference to this socket.
    */
    local_stream_socket& operator=(local_stream_socket&& other) noexcept
    {
        if (this != &other)
        {
            close();
            io_object::operator=(std::move(other));
        }
        return *this;
    }

    local_stream_socket(local_stream_socket const&)            = delete;
    local_stream_socket& operator=(local_stream_socket const&) = delete;

    /** Open the socket.

        Creates a Unix stream socket and associates it with
        the platform reactor.

        @param proto The protocol. Defaults to local_stream{}.

        @throws std::system_error on failure.
    */
    void open(local_stream proto = {});

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

        If the socket is not already open, it is opened automatically.

        @param ep The local endpoint (path) to connect to.

        @return An awaitable that completes with io_result<>.

        @throws std::system_error if the socket needs to be opened
            and the open fails.
    */
    auto connect(corosio::local_endpoint ep)
    {
        if (!is_open())
            open();
        return connect_awaitable(*this, ep);
    }

    /** Wait for the socket to become ready in a given direction.

        Suspends until the socket is ready for the requested
        direction, or an error condition is reported. No bytes
        are transferred.

        @param w The wait direction (read, write, or error).

        @return An awaitable that completes with `io_result<>`.

        @par Preconditions
        The socket must be open. This socket must outlive the
        returned awaitable.
    */
    [[nodiscard]] auto wait(wait_type w)
    {
        return wait_awaitable(*this, w);
    }

    /** Cancel any pending asynchronous operations.

        All outstanding operations complete with `errc::operation_canceled`.
        Check `ec == cond::canceled` for portable comparison.
    */
    void cancel();

    /** Get the native socket handle.

        Returns the underlying platform-specific socket descriptor.
        On POSIX systems this is an `int` file descriptor.

        @return The native socket handle, or an invalid sentinel
            if not open.
    */
    native_handle_type native_handle() const noexcept;

    /** Query the number of bytes available for reading.

        @return The number of bytes that can be read without blocking.

        @throws std::logic_error if the socket is not open.
        @throws std::system_error on ioctl failure.
    */
    std::size_t available() const;

    /** Release ownership of the native socket handle.

        Deregisters the socket from the backend and cancels pending
        operations without closing the descriptor. The caller takes
        ownership of the returned handle.

        @return The native handle.

        @throws std::logic_error if the socket is not open.

        @post is_open() == false
    */
    native_handle_type release();

    /** Disable sends or receives on the socket.

        Unix stream connections are full-duplex: each direction
        (send and receive) operates independently. This function
        allows you to close one or both directions without
        destroying the socket.

        @param what Determines what operations will no longer
            be allowed.

        @throws std::system_error on failure.
    */
    void shutdown(shutdown_type what);

    /** Shut down part or all of the socket (non-throwing).

        @param what Which direction to shut down.
        @param ec Set to the error code on failure.
    */
    void shutdown(shutdown_type what, std::error_code& ec) noexcept;

    /** Set a socket option.

        Applies a type-safe socket option to the underlying socket.
        The option type encodes the protocol level and option name.

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
            detail::throw_system_error(ec, "local_stream_socket::set_option");
    }

    /** Get a socket option.

        Retrieves the current value of a type-safe socket option.

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
            detail::throw_system_error(ec, "local_stream_socket::get_option");
        opt.resize(sz);
        return opt;
    }

    /** Assign an existing file descriptor to this socket.

        The socket must not already be open. The fd is adopted
        and registered with the platform reactor. Used by
        make_local_stream_pair() to wrap socketpair() fds.

        @param fd The file descriptor to adopt. Must be a valid,
            open, non-blocking Unix stream socket.

        @throws std::system_error on failure.
    */
    void assign(native_handle_type fd);

    /** Get the local endpoint of the socket.

        Returns the local address (path) to which the socket is bound.
        The endpoint is cached when the connection is established.

        @return The local endpoint, or a default endpoint if the socket
            is not connected.
    */
    corosio::local_endpoint local_endpoint() const noexcept;

    /** Get the remote endpoint of the socket.

        Returns the remote address (path) to which the socket is connected.
        The endpoint is cached when the connection is established.

        @return The remote endpoint, or a default endpoint if the socket
            is not connected.
    */
    corosio::local_endpoint remote_endpoint() const noexcept;

protected:
    local_stream_socket() noexcept = default;

    explicit local_stream_socket(handle h) noexcept : io_object(std::move(h)) {}

private:
    friend class local_stream_acceptor;

    void open_for_family(int family, int type, int protocol);

    inline implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_LOCAL_STREAM_SOCKET_HPP
