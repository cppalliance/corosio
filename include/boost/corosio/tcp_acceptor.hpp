//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TCP_ACCEPTOR_HPP
#define BOOST_COROSIO_TCP_ACCEPTOR_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/detail/op_base.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_socket.hpp>
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

/** An asynchronous TCP acceptor for coroutine I/O.

    This class provides asynchronous TCP accept operations that return
    awaitable types. The acceptor binds to a local endpoint and listens
    for incoming connections.

    Each accept operation participates in the affine awaitable protocol,
    ensuring coroutines resume on the correct executor.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. An acceptor must not have concurrent accept
    operations.

    @par Semantics
    Wraps the platform TCP listener. Operations dispatch to
    OS accept APIs via the io_context reactor.

    @par Example
    @code
    // Convenience constructor: open + configure + bind + listen
    io_context ioc;
    tcp_acceptor acc( ioc, endpoint( 8080 ) );

    tcp_socket peer( ioc );
    auto [ec] = co_await acc.accept( peer );
    if ( !ec ) {
        // peer is now a connected socket
        auto [ec2, n] = co_await peer.read_some( buf );
    }
    @endcode

    @par Example
    @code
    // Fine-grained setup
    tcp_acceptor acc( ioc );
    if ( auto ec = acc.open( tcp::v6() ) )
        return ec;
    acc.set_option( socket_option::reuse_address( true ) );
    acc.set_option( socket_option::v6_only( true ) );
    if ( auto ec = acc.bind( endpoint( ipv6_address::any(), 8080 ) ) )
        return ec;
    if ( auto ec = acc.listen() )
        return ec;
    @endcode
*/
class BOOST_COROSIO_DECL tcp_acceptor : public io_object
{
    struct wait_awaitable
        : detail::void_op_base<wait_awaitable>
    {
        tcp_acceptor& acc_;
        wait_type w_;

        wait_awaitable(tcp_acceptor& acc, wait_type w) noexcept
            : acc_(acc), w_(w) {}

        std::coroutine_handle<> dispatch(
            std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return acc_.get().wait(h, ex, w_, token_, &ec_);
        }
    };

    struct accept_awaitable
    {
        tcp_acceptor& acc_;
        tcp_socket& peer_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable io_object::implementation* peer_impl_ = nullptr;

        accept_awaitable(tcp_acceptor& acc, tcp_socket& peer) noexcept
            : acc_(acc)
            , peer_(peer)
        {
        }

        bool await_ready() const noexcept
        {
            // A pre-set ec_ means the initiator failed before
            // dispatch (e.g. a closed object).
            return static_cast<bool>(ec_) || token_.stop_requested();
        }

        [[nodiscard]] capy::io_result<> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(std::errc::operation_canceled)};

            if (!ec_ && peer_impl_)
                peer_.h_.reset(peer_impl_);
            return {ec_};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            return acc_.get().accept(
                h, env->executor, token_, &ec_, &peer_impl_);
        }
    };

    struct accept_value_awaitable
    {
        tcp_acceptor& acc_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable io_object::implementation* peer_impl_ = nullptr;

        explicit accept_value_awaitable(tcp_acceptor& acc) noexcept
            : acc_(acc)
        {
        }

        bool await_ready() const noexcept
        {
            // A pre-set ec_ means the initiator failed before
            // dispatch (e.g. a closed object).
            return static_cast<bool>(ec_) || token_.stop_requested();
        }

        [[nodiscard]] capy::io_result<tcp_socket> await_resume() noexcept
        {
            // The peer is built only on success: error paths must not
            // touch acc_.context(), which a moved-from acceptor lacks.
            if (token_.stop_requested())
                return {make_error_code(std::errc::operation_canceled),
                        tcp_socket()};

            if (ec_ || !peer_impl_)
                return {ec_, tcp_socket()};

            tcp_socket peer(acc_.context());
            peer.h_.reset(peer_impl_);
            return {ec_, std::move(peer)};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            return acc_.get().accept(
                h, env->executor, token_, &ec_, &peer_impl_);
        }
    };

public:
    /** Destructor.

        Closes the acceptor if open, cancelling any pending operations.
    */
    ~tcp_acceptor() override;

    /** Construct an acceptor from an execution context.

        @param ctx The execution context that will own this acceptor.
    */
    explicit tcp_acceptor(capy::execution_context& ctx);

    /** Convenience constructor: open + configure + bind + listen.

        Creates a fully-bound listening acceptor in a single
        expression, throwing the codes the piecewise `open()` +
        `set_option()` + `bind()` + `listen()` path reports. The
        address family is deduced from @p ep.

        Before binding, the constructor configures address reuse so
        a server can rebind its port immediately after a restart:
        `SO_REUSEADDR` on POSIX, `SO_EXCLUSIVEADDRUSE` on Windows
        ( where `SO_REUSEADDR` instead grants other sockets
        bind-over rights ). A second listener on an occupied
        endpoint therefore throws `errc::address_in_use` on every
        platform.

        @param ctx The execution context that will own this acceptor.
        @param ep The local endpoint to bind to.
        @param backlog The maximum pending connection queue length.

        @throws std::system_error on open, configuration, bind, or
            listen failure.
    */
    tcp_acceptor(capy::execution_context& ctx, endpoint ep, int backlog = 128);

    /** Construct an acceptor from an executor.

        The acceptor is associated with the executor's context.

        @param ex The executor whose context will own the acceptor.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, tcp_acceptor>) &&
        capy::Executor<Ex>
    explicit tcp_acceptor(Ex const& ex) : tcp_acceptor(ex.context())
    {
    }

    /** Convenience constructor from an executor.

        @param ex The executor whose context will own the acceptor.
        @param ep The local endpoint to bind to.
        @param backlog The maximum pending connection queue length.

        @throws std::system_error on open, configuration, bind, or
            listen failure.
    */
    template<class Ex>
        requires capy::Executor<Ex>
    tcp_acceptor(Ex const& ex, endpoint ep, int backlog = 128)
        : tcp_acceptor(ex.context(), ep, backlog)
    {
    }

    /** Move constructor.

        Transfers ownership of the acceptor resources.

        @param other The acceptor to move from.

        @pre No awaitables returned by @p other's methods exist.
        @pre The execution context associated with @p other must
            outlive this acceptor.
    */
    tcp_acceptor(tcp_acceptor&& other) noexcept : io_object(std::move(other)) {}

    /** Move assignment operator.

        Closes any existing acceptor and transfers ownership.

        @param other The acceptor to move from.

        @pre No awaitables returned by either `*this` or @p other's
            methods exist.
        @pre The execution context associated with @p other must
            outlive this acceptor.

        @return Reference to this acceptor.
    */
    tcp_acceptor& operator=(tcp_acceptor&& other) noexcept
    {
        if (this != &other)
        {
            close();
            h_ = std::move(other.h_);
        }
        return *this;
    }

    tcp_acceptor(tcp_acceptor const&)            = delete;
    tcp_acceptor& operator=(tcp_acceptor const&) = delete;

    /** Create the acceptor socket without binding or listening.

        Creates a TCP socket with dual-stack enabled for IPv6.
        Does not set SO_REUSEADDR — call `set_option` explicitly
        if needed.

        If the acceptor is already open, this function is a no-op.

        Failures such as descriptor exhaustion are normal runtime
        conditions and are reported through the returned error code.

        @param proto The protocol (IPv4 or IPv6). Defaults to
            `tcp::v4()`.

        @par Example
        @code
        if (auto ec = acc.open( tcp::v6() ))
            return;  // report the error
        acc.set_option( socket_option::reuse_address( true ) );
        if (auto ec = acc.bind( endpoint( ipv6_address::any(), 8080 ) ))
            return;
        if (auto ec = acc.listen())
            return;
        @endcode

        @see bind, listen

        @return The error code, empty on success.
    */
    [[nodiscard]] std::error_code open(tcp proto = tcp::v4()) noexcept;

    /** Bind to a local endpoint.

        The acceptor must be open. Binds the socket to @p ep and
        caches the resolved local endpoint (useful when port 0 is
        used to request an ephemeral port).

        @param ep The local endpoint to bind to.

        @return An error code indicating success or the reason for
            failure.

        @par Error Conditions
        @li `errc::address_in_use`: The endpoint is already in use.
        @li `errc::address_not_available`: The address is not available
            on any local interface.
        @li `errc::permission_denied`: Insufficient privileges to bind
            to the endpoint (e.g., privileged port).

        A closed acceptor reports `errc::bad_file_descriptor`.
    */
    [[nodiscard]] std::error_code bind(endpoint ep) noexcept;

    /** Start listening for incoming connections.

        The acceptor must be open and bound. Registers the acceptor
        with the platform reactor.

        @param backlog The maximum length of the queue of pending
            connections. Defaults to 128.

        @return An error code indicating success or the reason for
            failure.

        A closed acceptor reports `errc::bad_file_descriptor`.
    */
    [[nodiscard]] std::error_code listen(int backlog = 128) noexcept;

    /** Close the acceptor.

        Releases acceptor resources. Any pending operations complete
        with `errc::operation_canceled`.
    */
    void close() noexcept;

    /** Check if the acceptor is listening.

        @return `true` if the acceptor is open and listening.
    */
    bool is_open() const noexcept
    {
        return h_ && get().is_open();
    }

    /** Initiate an asynchronous accept operation.

        Accepts an incoming connection and initializes the provided
        socket with the new connection. The acceptor must be listening
        before calling this function.

        The operation supports cancellation via `std::stop_token` through
        the affine awaitable protocol. If the associated stop token is
        triggered, the operation completes immediately with
        `errc::operation_canceled`.

        @param peer The socket to receive the accepted connection. Any
            existing connection on this socket will be closed.

        @return An awaitable that completes with `io_result<>`.
            Returns success on successful accept, or an error code on
            failure including:
            - operation_canceled: Cancelled via stop_token or cancel().
                Check `ec == cond::canceled` for portable comparison.

        A closed acceptor completes with `errc::bad_file_descriptor`.

        @par Preconditions
        The peer socket must be associated with the same execution context.

        Both this acceptor and @p peer must outlive the returned
        awaitable.

        @par Example
        @code
        tcp_socket peer(ioc);
        auto [ec] = co_await acc.accept(peer);
        if (ec)
            co_return;
        auto [wec, n] = co_await peer.write_some(buffer);
        @endcode

        @see accept()
    */
    [[nodiscard]] auto accept(tcp_socket& peer)
    {
        accept_awaitable aw(*this, peer);
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /** Initiate an asynchronous accept operation, returning the peer.

        Accepts an incoming connection and returns a newly constructed
        socket for it, associated with this acceptor's execution context.
        The acceptor must be listening before calling this function.

        The caller does not pre-construct the peer socket; the returned
        socket shares this acceptor's execution context.

        The operation supports cancellation via `std::stop_token` through
        the affine awaitable protocol. If the associated stop token is
        triggered, the operation completes immediately with
        `errc::operation_canceled`.

        @return An awaitable that completes with `io_result<tcp_socket>`.
            On success the payload is the connected peer socket; on failure
            (including cancellation) the error code is set and the payload
            socket is unconnected. Errors include:
            - operation_canceled: Cancelled via stop_token or cancel().
                Check `ec == cond::canceled` for portable comparison.

        A closed acceptor completes with `errc::bad_file_descriptor`.
        On failure the returned socket is default-constructed and
        may only be destroyed or assigned.

        @par Preconditions
        This acceptor must outlive the returned awaitable.

        @par Example
        @code
        auto [ec, peer] = co_await acc.accept();
        if (ec)
            co_return;
        auto [wec, n] = co_await peer.write_some(buffer);
        @endcode

        @see accept(tcp_socket&)
    */
    [[nodiscard]] auto accept()
    {
        accept_value_awaitable aw(*this);
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /** Wait for an incoming connection or readiness condition.

        Suspends until the listen socket is ready in the
        requested direction, or an error condition is reported.
        For `wait_type::read`, completion signals that a
        subsequent @ref accept will succeed without blocking; a
        connection already queued when the wait begins completes
        it immediately. No connection is consumed.

        @note `wait_type::write` is not usable on an acceptor:
        writability carries no meaning for a listening socket, so
        the wait fails with `errc::operation_not_supported` on
        every backend.

        @param w The wait direction.

        @return An awaitable that completes with `io_result<>`.

        A closed acceptor completes with `errc::bad_file_descriptor`.

        @par Preconditions
        This acceptor must outlive the returned awaitable.
    */
    [[nodiscard]] auto wait(wait_type w)
    {
        wait_awaitable aw(*this, w);
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /** Cancel any pending asynchronous operations.

        All outstanding operations complete with `errc::operation_canceled`.
        Check `ec == cond::canceled` for portable comparison.
    */
    void cancel() noexcept;

    /** Get the native socket handle.

        Returns the underlying platform-specific socket descriptor.
        On POSIX systems this is an `int` file descriptor.
        On Windows this is a `SOCKET` handle.

        @return The native socket handle, or -1/INVALID_SOCKET if not open.

        @par Preconditions
        None. May be called on closed acceptors.
    */
    native_handle_type native_handle() const noexcept;

    /** Assign an existing native socket to this acceptor.

        Adopts a listening socket created outside the library —
        received from a service manager, inherited, or made natively —
        and registers it with the backend. The socket must be a
        listening stream socket in the `AF_INET` or `AF_INET6` family.
        Adoption never alters the descriptor's flags or options: on
        POSIX the fd must already be non-blocking, and on Windows the
        socket must be overlapped-capable.

        Adoption does not verify listen state; @ref accept reports the
        error if the socket is not listening.

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

    /** Release ownership of the native socket handle.

        Deregisters the socket from the backend and cancels pending
        operations without closing the descriptor. The caller takes
        ownership of the returned handle.

        @return The native handle.

        @throws std::system_error `errc::bad_file_descriptor` if the
            acceptor is not open.

        @post is_open() == false
    */
    native_handle_type release();

    /** Get the local endpoint of the acceptor.

        Returns the local address and port to which the acceptor is bound.
        This is useful when binding to port 0 (ephemeral port) to discover
        the OS-assigned port number. The endpoint is cached when bind()
        is called.

        @return The local endpoint, or a default endpoint (0.0.0.0:0) if
            the acceptor is not open.

        @par Thread Safety
        The cached endpoint value is set during bind() and cleared
        during close(). This function may be called concurrently with
        accept operations, but must not be called concurrently with
        bind() or close().
    */
    endpoint local_endpoint() const noexcept;

    /** Set a socket option on the acceptor.

        Applies a type-safe socket option to the underlying listening
        socket. The socket must be open (via `open()` or `listen()`).
        This is useful for setting options between `open()` and
        `listen()`, such as `socket_option::reuse_port`.

        @par Example
        @code
        if ( auto ec = acc.open( tcp::v6() ) )
            return ec;
        acc.set_option( socket_option::reuse_port( true ) );
        if ( auto ec = acc.bind( endpoint( ipv6_address::any(), 8080 ) ) )
            return ec;
        if ( auto ec = acc.listen() )
            return ec;
        @endcode

        @param opt The option to set.

        @throws std::system_error `errc::bad_file_descriptor` if the
            acceptor is not open; otherwise thrown on failure.
    */
    template<class Option>
    void set_option(Option const& opt)
    {
        if (!is_open())
            detail::throw_system_error(
                make_error_code(std::errc::bad_file_descriptor),
                "tcp_acceptor::set_option");
        std::error_code ec = get().set_option(
            Option::level(), Option::name(), opt.data(), opt.size());
        if (ec)
            detail::throw_system_error(ec, "tcp_acceptor::set_option");
    }

    /** Get a socket option from the acceptor.

        Retrieves the current value of a type-safe socket option.

        @par Example
        @code
        auto opt = acc.get_option<socket_option::reuse_address>();
        @endcode

        @return The current option value.

        @throws std::system_error `errc::bad_file_descriptor` if the
            acceptor is not open; otherwise thrown on failure.
    */
    template<class Option>
    Option get_option() const
    {
        if (!is_open())
            detail::throw_system_error(
                make_error_code(std::errc::bad_file_descriptor),
                "tcp_acceptor::get_option");
        Option opt{};
        std::size_t sz = opt.size();
        std::error_code ec =
            get().get_option(Option::level(), Option::name(), opt.data(), &sz);
        if (ec)
            detail::throw_system_error(ec, "tcp_acceptor::get_option");
        opt.resize(sz);
        return opt;
    }

    /** Define backend hooks for TCP acceptor operations.

        Platform backends derive from this to implement
        accept, endpoint query, open-state checks, cancellation,
        and socket-option management.
    */
    struct implementation : io_object::implementation
    {
        /// Initiate an asynchronous accept operation.
        virtual std::coroutine_handle<> accept(
            std::coroutine_handle<>,
            capy::executor_ref,
            std::stop_token,
            std::error_code*,
            io_object::implementation**) = 0;

        /** Initiate an asynchronous wait for acceptor readiness.

            Completes when the listen socket becomes ready for
            the specified direction (typically `wait_type::read`
            for an incoming connection), or an error condition is
            reported. No connection is consumed.
        */
        virtual std::coroutine_handle<> wait(
            std::coroutine_handle<> h,
            capy::executor_ref ex,
            wait_type w,
            std::stop_token token,
            std::error_code* ec) = 0;

        /// Returns the cached local endpoint.
        virtual endpoint local_endpoint() const noexcept = 0;

        /// Return true if the acceptor has a kernel resource open.
        virtual bool is_open() const noexcept = 0;

        /// Return the native handle, or the platform sentinel if closed.
        virtual native_handle_type native_handle() const noexcept = 0;

        /// Release and return the native handle without closing.
        virtual native_handle_type release_socket() noexcept = 0;

        /** Cancel any pending asynchronous operations.

            All outstanding operations complete with operation_canceled error.
        */
        virtual void cancel() noexcept = 0;

        /** Set a socket option.

            @param level The protocol level.
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

            @param level The protocol level.
            @param optname The option name.
            @param data Pointer to receive the option value.
            @param size On entry, the size of the buffer. On exit,
                the size of the option value.
            @return Error code on failure, empty on success.
        */
        virtual std::error_code
        get_option(int level, int optname, void* data, std::size_t* size)
            const noexcept = 0;
    };

protected:
    explicit tcp_acceptor(handle h) noexcept : io_object(std::move(h)) {}

    /// Transfer accepted peer impl to the peer socket.
    static void
    reset_peer_impl(tcp_socket& peer, io_object::implementation* impl) noexcept
    {
        if (impl)
            peer.h_.reset(impl);
    }

private:
    inline implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif
