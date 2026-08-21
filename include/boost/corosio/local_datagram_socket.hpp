//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_LOCAL_DATAGRAM_SOCKET_HPP
#define BOOST_COROSIO_LOCAL_DATAGRAM_SOCKET_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/detail/op_base.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/corosio/detail/buffer_param.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_datagram.hpp>
#include <boost/corosio/message_flags.hpp>
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

/** An asynchronous Unix datagram socket for coroutine I/O.

    This class provides asynchronous Unix domain datagram socket
    operations that return awaitable types. Each operation
    participates in the affine awaitable protocol, ensuring
    coroutines resume on the correct executor.

    Supports two modes of operation:

    @li **Connectionless:** each send_to() specifies a destination
        endpoint, and each recv_from() captures the source. The
        socket must be opened (and optionally bound) before I/O.

    @li **Connected:** call connect() to set a default peer,
        then use send()/recv() without endpoint arguments. The
        kernel filters incoming datagrams to those from the
        connected peer.

    @note Not available on Windows. Windows does not support
        AF_UNIX datagram sockets (SOCK_DGRAM). Attempting to
        open this socket on Windows will fail.

    @par Cancellation
    All asynchronous operations support cancellation through
    `std::stop_token` via the affine protocol, or explicitly
    through cancel(). Cancelled operations complete with
    `capy::cond::canceled`. Datagram sends and receives are
    atomic — there is no partial progress on cancellation.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. A socket must not have concurrent
    operations of the same type (e.g., two simultaneous
    recv_from). One send and one recv may be in flight
    simultaneously. Note that recv and recv_from share the
    same internal read slot, so they must not overlap; likewise
    send and send_to share the write slot.

    @par Example
    @code
    // Connectionless
    local_datagram_socket sender(ioc);
    if (auto ec = sender.open())
        co_return;
    if (auto ec = sender.bind(local_endpoint("/tmp/sender.sock")))
        co_return;
    auto [ec, n] = co_await sender.send_to(
        capy::const_buffer("hello", 5),
        local_endpoint("/tmp/receiver.sock"));
    if (ec)
        co_return;

    // Connected
    local_datagram_socket sock(ioc);
    auto [cec] = co_await sock.connect(local_endpoint("/tmp/peer.sock"));
    if (cec)
        co_return;
    auto [ec2, n2] = co_await sock.send(
        capy::const_buffer("hi", 2));
    if (ec2)
        co_return;
    @endcode
*/
class BOOST_COROSIO_DECL local_datagram_socket : public io_object
{
public:
    /// The shutdown direction type used by shutdown().
    using shutdown_type = corosio::shutdown_type;
    using enum corosio::shutdown_type;

    /** Define backend hooks for local datagram socket operations.

        Platform backends (epoll, kqueue, select) derive from this
        to implement datagram I/O, connection, and option management.
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
            corosio::local_endpoint dest,
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
            corosio::local_endpoint* source,
            int flags,
            std::stop_token token,
            std::error_code* ec,
            std::size_t* bytes_out) = 0;

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
            corosio::local_endpoint ep,
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
            @param flags Message flags (e.g. MSG_PEEK).
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

        /// Shut down part or all of the socket.
        virtual std::error_code shutdown(shutdown_type what) noexcept = 0;

        /// Return the platform socket descriptor.
        virtual native_handle_type native_handle() const noexcept = 0;

        /** Release ownership of the socket descriptor.

            The implementation deregisters from the reactor and cancels
            pending operations. The caller takes ownership of the
            returned descriptor.

            @return The native handle, or an invalid sentinel if
                not open.
        */
        virtual native_handle_type release_socket() noexcept = 0;

        /** Request cancellation of pending asynchronous operations.

            All outstanding operations complete with operation_canceled
            error. Check ec == cond::canceled for portable comparison.
        */
        virtual void cancel() noexcept = 0;

        /** Set a socket option.

            @param level The protocol level (e.g. SOL_SOCKET).
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

            @param level The protocol level (e.g. SOL_SOCKET).
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
        virtual corosio::local_endpoint local_endpoint() const noexcept = 0;

        /// Return the cached remote endpoint (connected mode).
        virtual corosio::local_endpoint remote_endpoint() const noexcept = 0;

        /** Bind the socket to a local endpoint.

            @param ep The local endpoint to bind to.
            @return Error code on failure, empty on success.
        */
        virtual std::error_code
        bind(corosio::local_endpoint ep) noexcept = 0;
    };

    /** Represent the awaitable returned by @ref send_to.

        Captures the destination endpoint and buffer, then dispatches
        to the backend implementation on suspension.
    */
    struct send_to_awaitable
        : detail::bytes_op_base<send_to_awaitable>
    {
        local_datagram_socket& s_;
        buffer_param buf_;
        corosio::local_endpoint dest_;
        int flags_;

        send_to_awaitable(
            local_datagram_socket& s, buffer_param buf,
            corosio::local_endpoint dest, int flags = 0) noexcept
            : s_(s), buf_(buf), dest_(dest), flags_(flags) {}

        std::coroutine_handle<> dispatch(
            std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return s_.get().send_to(
                h, ex, buf_, dest_, flags_, token_, &ec_, &bytes_);
        }
    };

    /** Represent the awaitable returned by @ref recv_from.

        Captures the source endpoint reference and buffer, then
        dispatches to the backend implementation on suspension.
    */
    struct recv_from_awaitable
        : detail::bytes_op_base<recv_from_awaitable>
    {
        local_datagram_socket& s_;
        buffer_param buf_;
        corosio::local_endpoint& source_;
        int flags_;

        recv_from_awaitable(
            local_datagram_socket& s, buffer_param buf,
            corosio::local_endpoint& source, int flags = 0) noexcept
            : s_(s), buf_(buf), source_(source), flags_(flags) {}

        std::coroutine_handle<> dispatch(
            std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return s_.get().recv_from(
                h, ex, buf_, &source_, flags_, token_, &ec_, &bytes_);
        }
    };

    /** Represent the awaitable returned by @ref connect.

        Captures the target endpoint, then dispatches to the
        backend implementation on suspension.
    */
    struct connect_awaitable
        : detail::void_op_base<connect_awaitable>
    {
        local_datagram_socket& s_;
        corosio::local_endpoint endpoint_;

        connect_awaitable(
            local_datagram_socket& s,
            corosio::local_endpoint ep) noexcept
            : s_(s), endpoint_(ep) {}

        std::coroutine_handle<> dispatch(
            std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return s_.get().connect(
                h, ex, endpoint_, token_, &ec_);
        }
    };

    /// Represent the awaitable returned by @ref wait.
    struct wait_awaitable
        : detail::void_op_base<wait_awaitable>
    {
        local_datagram_socket& s_;
        wait_type w_;

        wait_awaitable(local_datagram_socket& s, wait_type w) noexcept
            : s_(s), w_(w) {}

        std::coroutine_handle<> dispatch(
            std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return s_.get().wait(h, ex, w_, token_, &ec_);
        }
    };

    /** Represent the awaitable returned by @ref send.

        Captures the buffer, then dispatches to the backend
        implementation on suspension. Requires a prior connect().
    */
    struct send_awaitable
        : detail::bytes_op_base<send_awaitable>
    {
        local_datagram_socket& s_;
        buffer_param buf_;
        int flags_;

        send_awaitable(
            local_datagram_socket& s, buffer_param buf,
            int flags = 0) noexcept
            : s_(s), buf_(buf), flags_(flags) {}

        std::coroutine_handle<> dispatch(
            std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return s_.get().send(
                h, ex, buf_, flags_, token_, &ec_, &bytes_);
        }
    };

    /** Represent the awaitable returned by @ref recv.

        Captures the buffer, then dispatches to the backend
        implementation on suspension. Requires a prior connect().
    */
    struct recv_awaitable
        : detail::bytes_op_base<recv_awaitable>
    {
        local_datagram_socket& s_;
        buffer_param buf_;
        int flags_;

        recv_awaitable(
            local_datagram_socket& s, buffer_param buf,
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
    ~local_datagram_socket() override;

    /** Construct a socket from an execution context.

        @param ctx The execution context that will own this socket.
    */
    explicit local_datagram_socket(capy::execution_context& ctx);

    /** Construct a socket from an executor.

        The socket is associated with the executor's context.

        @param ex The executor whose context will own the socket.
    */
    template<class Ex>
        requires(
            !std::same_as<std::remove_cvref_t<Ex>, local_datagram_socket>) &&
        capy::Executor<Ex>
    explicit local_datagram_socket(Ex const& ex)
        : local_datagram_socket(ex.context())
    {
    }

    /** Move constructor.

        Transfers ownership of the socket resources.

        @param other The socket to move from.
    */
    local_datagram_socket(local_datagram_socket&& other) noexcept
        : io_object(std::move(other))
    {
    }

    /** Move assignment operator.

        Closes any existing socket and transfers ownership.

        @param other The socket to move from.
        @return Reference to this socket.
    */
    local_datagram_socket& operator=(local_datagram_socket&& other) noexcept
    {
        if (this != &other)
        {
            close();
            io_object::operator=(std::move(other));
        }
        return *this;
    }

    local_datagram_socket(local_datagram_socket const&)            = delete;
    local_datagram_socket& operator=(local_datagram_socket const&) = delete;

    /** Open the socket.

        Creates a Unix datagram socket and associates it with
        the platform reactor.

        Failures such as descriptor exhaustion are normal runtime
        conditions and are reported through the returned error code.
        Opening an already-open socket is a no-op that reports
        success.

        @param proto The protocol. Defaults to local_datagram{}.

        @return The error code, empty on success.
    */
    [[nodiscard]] std::error_code open(local_datagram proto = {}) noexcept;

    /** Close the socket.

        Cancels any pending asynchronous operations and releases
        the underlying file descriptor. Has no effect if the
        socket is not open.

        @post is_open() == false
    */
    void close() noexcept;

    /** Check if the socket is open.

        @return `true` if the socket holds a valid file descriptor,
            `false` otherwise.
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

        Associates the socket with a local address (filesystem path).
        Required before calling recv_from in connectionless mode.

        @param ep The local endpoint to bind to.

        @return Error code on failure, empty on success.

        A closed socket reports `errc::bad_file_descriptor`.
    */
    [[nodiscard]] std::error_code bind(corosio::local_endpoint ep) noexcept;

    /** Initiate an asynchronous connect to set the default peer.

        If the socket is not already open, it is opened automatically.
        After successful completion, send()/recv() may be used
        without specifying an endpoint.

        @param ep The remote endpoint to connect to.

        @par Cancellation
        Supports cancellation via the awaitable's stop_token or by
        calling cancel(). On cancellation, yields
        `capy::cond::canceled`.

        @return An awaitable that completes with io_result<>.

        If the socket needs to be opened and the open fails, the
        awaitable completes immediately with that error.
    */
    [[nodiscard]] auto connect(corosio::local_endpoint ep)
    {
        connect_awaitable aw(*this, ep);
        if (!is_open())
            aw.ec_ = open();
        return aw;
    }

    /** Wait for the socket to become ready in a given direction.

        Suspends until the socket is ready for the requested
        direction, or an error condition is reported. No bytes
        are transferred.

        @param w The wait direction (read, write, or error).

        @return An awaitable that completes with `io_result<>`.

        A closed socket completes with `errc::bad_file_descriptor`.

        @par Preconditions
        This socket must outlive the returned awaitable.
    */
    [[nodiscard]] auto wait(wait_type w)
    {
        return wait_awaitable(*this, w);
    }

    /** Send a datagram to the specified destination.

        Completes when the entire datagram has been accepted
        by the kernel. The bytes_transferred value equals the
        datagram size on success.

        @param buf The buffer containing data to send.
        @param dest The destination endpoint.

        @par Cancellation
        Supports cancellation via stop_token or cancel().

        @return An awaitable that completes with
            io_result<std::size_t>.

        A closed socket reports `errc::bad_file_descriptor`.
    */
    template<capy::ConstBufferSequence Buffers>
    [[nodiscard]] auto send_to(
        Buffers const& buf,
        corosio::local_endpoint dest,
        corosio::message_flags flags)
    {
        send_to_awaitable aw(*this, buf, dest, static_cast<int>(flags));
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /// @overload
    template<capy::ConstBufferSequence Buffers>
    [[nodiscard]] auto send_to(Buffers const& buf, corosio::local_endpoint dest)
    {
        return send_to(buf, dest, corosio::message_flags::none);
    }

    /** Receive a datagram and capture the sender's endpoint.

        Completes when one datagram has been received. The
        bytes_transferred value is the number of bytes copied
        into the buffer. If the buffer is smaller than the
        datagram, excess bytes are discarded (datagram
        semantics).

        @param buf The buffer to receive data into.
        @param source Reference to an endpoint that will be set to
            the sender's address on successful completion.
        @param flags Message flags (e.g. message_flags::peek).

        @par Cancellation
        Supports cancellation via stop_token or cancel().

        @return An awaitable that completes with
            io_result<std::size_t>.

        A closed socket reports `errc::bad_file_descriptor`.
    */
    template<capy::MutableBufferSequence Buffers>
    [[nodiscard]] auto recv_from(
        Buffers const& buf,
        corosio::local_endpoint& source,
        corosio::message_flags flags)
    {
        recv_from_awaitable aw(*this, buf, source, static_cast<int>(flags));
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /// @overload
    template<capy::MutableBufferSequence Buffers>
    [[nodiscard]] auto recv_from(Buffers const& buf, corosio::local_endpoint& source)
    {
        return recv_from(buf, source, corosio::message_flags::none);
    }

    /** Send a datagram to the connected peer.

        @pre connect() has been called successfully.

        @param buf The buffer containing data to send.
        @param flags Message flags.

        @par Cancellation
        Supports cancellation via stop_token or cancel().

        @return An awaitable that completes with
            io_result<std::size_t>.

        A closed socket reports `errc::bad_file_descriptor`.
    */
    template<capy::ConstBufferSequence Buffers>
    [[nodiscard]] auto send(Buffers const& buf, corosio::message_flags flags)
    {
        send_awaitable aw(*this, buf, static_cast<int>(flags));
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /// @overload
    template<capy::ConstBufferSequence Buffers>
    [[nodiscard]] auto send(Buffers const& buf)
    {
        return send(buf, corosio::message_flags::none);
    }

    /** Receive a datagram from the connected peer.

        @pre connect() has been called successfully.

        @param buf The buffer to receive data into.
        @param flags Message flags (e.g. message_flags::peek).

        @par Cancellation
        Supports cancellation via stop_token or cancel().

        @return An awaitable that completes with
            io_result<std::size_t>.

        A closed socket reports `errc::bad_file_descriptor`.
    */
    template<capy::MutableBufferSequence Buffers>
    [[nodiscard]] auto recv(Buffers const& buf, corosio::message_flags flags)
    {
        recv_awaitable aw(*this, buf, static_cast<int>(flags));
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /// @overload
    template<capy::MutableBufferSequence Buffers>
    [[nodiscard]] auto recv(Buffers const& buf)
    {
        return recv(buf, corosio::message_flags::none);
    }

    /** Cancel any pending asynchronous operations.

        All outstanding operations complete with
        errc::operation_canceled. Check ec == cond::canceled
        for portable comparison.
    */
    void cancel() noexcept;

    /** Get the native socket handle.

        @return The native socket handle, or -1 if not open.
    */
    native_handle_type native_handle() const noexcept;

    /** Release ownership of the native socket handle.

        Deregisters the socket from the reactor and cancels pending
        operations without closing the fd. The caller takes ownership
        of the returned descriptor.

        @return The native handle.

        @throws std::system_error `errc::bad_file_descriptor` if the
            socket is not open.
    */
    native_handle_type release();

    /** Query the number of bytes available for reading.

        @return The number of bytes that can be read without blocking.

        @throws std::system_error `errc::bad_file_descriptor` if the
            socket is not open; otherwise thrown on ioctl failure.
    */
    std::size_t available() const;

    /** Shut down part or all of the socket.

        Failures such as an unconnected socket are normal runtime
        conditions and are reported through the returned error
        code. A closed socket reports `errc::bad_file_descriptor`.

        @param what Which direction to shut down.

        @return The error code, empty on success.
    */
    [[nodiscard]] std::error_code shutdown(shutdown_type what) noexcept;

    /** Set a socket option.

        @tparam Option A socket option type that provides static
            `level()` and `name()` members, and `data()` / `size()`
            accessors for the option value.

        @param opt The option to set.

        @throws std::system_error `errc::bad_file_descriptor` if the
            socket is not open; otherwise thrown on failure.
    */
    template<class Option>
    void set_option(Option const& opt)
    {
        if (!is_open())
            detail::throw_system_error(
                make_error_code(std::errc::bad_file_descriptor),
                "local_datagram_socket::set_option");
        std::error_code ec = get().set_option(
            Option::level(), Option::name(), opt.data(), opt.size());
        if (ec)
            detail::throw_system_error(
                ec, "local_datagram_socket::set_option");
    }

    /** Get a socket option.

        @tparam Option A socket option type that provides static
            `level()` and `name()` members, `data()` / `size()`
            accessors, and a `resize()` member.

        @return The current option value.

        @throws std::system_error `errc::bad_file_descriptor` if the
            socket is not open; otherwise thrown on failure.
    */
    template<class Option>
    Option get_option() const
    {
        if (!is_open())
            detail::throw_system_error(
                make_error_code(std::errc::bad_file_descriptor),
                "local_datagram_socket::get_option");
        Option opt{};
        std::size_t sz = opt.size();
        std::error_code ec =
            get().get_option(Option::level(), Option::name(), opt.data(), &sz);
        if (ec)
            detail::throw_system_error(
                ec, "local_datagram_socket::get_option");
        opt.resize(sz);
        return opt;
    }

    /** Assign an existing native socket to this object.

        Adopts a Unix domain datagram socket created outside the
        library — from `socketpair()`, received over `SCM_RIGHTS`,
        or made natively — and registers it with the backend. The
        socket must be a datagram socket in the `AF_UNIX` family.
        Adoption never alters the descriptor's flags or options; the
        fd must already be non-blocking.

        If this object is already open, pending operations complete
        with `errc::operation_canceled` and the held socket is
        closed before the new one is adopted.

        @par Exception Safety
        Strong guarantee on validation failure: the object is
        unchanged. If backend registration fails, the object either
        retains its previous socket or is left closed, depending on
        the backend. In all failure cases the caller retains
        ownership of `fd`.

        @param fd The native socket to adopt. On success the object
            owns it and will close it.

        @return The error code, empty on success. Validation and
            registration failures are normal runtime conditions when
            adopting foreign descriptors.
    */
    [[nodiscard]] std::error_code assign(native_handle_type fd) noexcept;

    /** Get the local endpoint of the socket.

        @return The local endpoint, or a default endpoint if not bound.
    */
    corosio::local_endpoint local_endpoint() const noexcept;

    /** Get the remote endpoint of the socket.

        Returns the address of the connected peer.

        @return The remote endpoint, or a default endpoint if
            not connected.
    */
    corosio::local_endpoint remote_endpoint() const noexcept;

protected:
    /// Default-construct (for derived types).
    local_datagram_socket() noexcept = default;

    /// Construct from a pre-built handle.
    explicit local_datagram_socket(handle h) noexcept
        : io_object(std::move(h))
    {
    }

private:
    [[nodiscard]] std::error_code
    open_for_family(int family, int type, int protocol) noexcept;

    inline implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_LOCAL_DATAGRAM_SOCKET_HPP
