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

    Connectionless mode: each send_to specifies a destination
    endpoint, and each recv_from captures the source endpoint.
    The socket must be opened (and optionally bound) before I/O.

    Connected mode: call connect() to set a default peer,
    then use send()/recv() without endpoint arguments. The kernel
    filters incoming datagrams to those from the connected peer.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. A socket must not have concurrent
    operations of the same type (e.g., two simultaneous recv_from).
    One send_to and one recv_from may be in flight simultaneously.
    Note that recv and recv_from share the same internal read slot,
    so they must not overlap; likewise send and send_to share the
    write slot.
*/
class BOOST_COROSIO_DECL local_datagram_socket : public io_object
{
public:
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
            @param flags Message flags (e.g. MSG_DONTROUTE).
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
            @param flags Message flags (e.g. MSG_PEEK).
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
            @param flags Message flags (e.g. MSG_DONTROUTE).
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

        /// Shut down part or all of the socket.
        virtual std::error_code shutdown(shutdown_type what) noexcept = 0;

        /// Return the platform socket descriptor.
        virtual native_handle_type native_handle() const noexcept = 0;

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

        @param proto The protocol. Defaults to local_datagram{}.

        @throws std::system_error on failure.
    */
    void open(local_datagram proto = {});

    /// Close the socket.
    void close();

    /// Check if the socket is open.
    bool is_open() const noexcept
    {
        return h_ && get().native_handle() >= 0;
    }

    /** Bind the socket to a local endpoint.

        Associates the socket with a local address (filesystem path).
        Required before calling recv_from in connectionless mode.

        @param ep The local endpoint to bind to.

        @return Error code on failure, empty on success.

        @throws std::logic_error if the socket is not open.
    */
    std::error_code bind(corosio::local_endpoint ep);

    /** Initiate an asynchronous connect to set the default peer.

        If the socket is not already open, it is opened automatically.

        @param ep The remote endpoint to connect to.

        @par Cancellation
        Supports cancellation via the awaitable's stop_token or by
        calling @ref cancel. On cancellation, yields
        `errc::operation_canceled`.

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

    /** Send a datagram to the specified destination.

        @param buf The buffer containing data to send.
        @param dest The destination endpoint.

        @par Cancellation
        Supports cancellation via the awaitable's stop_token or by
        calling @ref cancel. On cancellation, yields
        `errc::operation_canceled`.

        @return An awaitable that completes with
            io_result<std::size_t>.

        @throws std::logic_error if the socket is not open.
    */
    template<capy::ConstBufferSequence Buffers>
    auto send_to(
        Buffers const& buf,
        corosio::local_endpoint dest,
        corosio::message_flags flags)
    {
        if (!is_open())
            detail::throw_logic_error("send_to: socket not open");
        return send_to_awaitable(
            *this, buf, dest, static_cast<int>(flags));
    }

    /// @overload
    template<capy::ConstBufferSequence Buffers>
    auto send_to(Buffers const& buf, corosio::local_endpoint dest)
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
            io_result<std::size_t>.

        @throws std::logic_error if the socket is not open.
    */
    template<capy::MutableBufferSequence Buffers>
    auto recv_from(
        Buffers const& buf,
        corosio::local_endpoint& source,
        corosio::message_flags flags)
    {
        if (!is_open())
            detail::throw_logic_error("recv_from: socket not open");
        return recv_from_awaitable(
            *this, buf, source, static_cast<int>(flags));
    }

    /// @overload
    template<capy::MutableBufferSequence Buffers>
    auto recv_from(Buffers const& buf, corosio::local_endpoint& source)
    {
        return recv_from(buf, source, corosio::message_flags::none);
    }

    /** Send a datagram to the connected peer.

        @param buf The buffer containing data to send.
        @param flags Message flags.

        @par Cancellation
        Supports cancellation via the awaitable's stop_token or by
        calling @ref cancel. On cancellation, yields
        `errc::operation_canceled`.

        @return An awaitable that completes with
            io_result<std::size_t>.

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
            io_result<std::size_t>.

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

    /** Cancel any pending asynchronous operations.

        All outstanding operations complete with
        errc::operation_canceled. Check ec == cond::canceled
        for portable comparison.
    */
    void cancel();

    /** Get the native socket handle.

        @return The native socket handle, or -1 if not open.
    */
    native_handle_type native_handle() const noexcept;

    /** Release ownership of the native socket handle.

        Deregisters the socket from the reactor and cancels pending
        operations without closing the fd. The caller takes ownership
        of the returned descriptor.

        @return The native handle.

        @throws std::logic_error if the socket is not open.
    */
    native_handle_type release();

    /** Query the number of bytes available for reading.

        @return The number of bytes that can be read without blocking.

        @throws std::logic_error if the socket is not open.
        @throws std::system_error on ioctl failure.
    */
    std::size_t available() const;

    /** Shut down part or all of the socket (best-effort).

        Calls `::shutdown` on the underlying descriptor when open.
        Errors from the syscall (such as `ENOTCONN` on a peer that
        already closed) are swallowed because they are typically
        unhelpful at this layer; if the socket is not open, the call
        is a no-op. To observe errors, use the
        @ref shutdown(shutdown_type,std::error_code&) overload.

        @param what Which direction to shut down.
    */
    void shutdown(shutdown_type what);

    /** Shut down part or all of the socket (non-throwing).

        @param what Which direction to shut down.
        @param ec Set to the error code on failure.
    */
    void shutdown(shutdown_type what, std::error_code& ec) noexcept;

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
            detail::throw_system_error(
                ec, "local_datagram_socket::set_option");
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
            detail::throw_system_error(
                ec, "local_datagram_socket::get_option");
        opt.resize(sz);
        return opt;
    }

    /** Assign an existing file descriptor to this socket.

        The fd is adopted and registered with the platform reactor.

        @param fd The file descriptor to adopt.

        @par Preconditions
        The socket must not already be open.

        @throws std::logic_error if the precondition is violated
            (the socket is already open).
        @throws std::system_error on any other failure (e.g. the
            fd is not an AF_UNIX datagram socket, or backend
            configuration fails).
    */
    void assign(int fd);

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
    void open_for_family(int family, int type, int protocol);

    inline implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_LOCAL_DATAGRAM_SOCKET_HPP
