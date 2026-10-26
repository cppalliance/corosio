//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TCP_ACCEPTOR_HPP
#define BOOST_COROSIO_TCP_ACCEPTOR_HPP

#include <boost/corosio/family.hpp>
#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/native_handle.hpp>
#include <boost/corosio/detail/op_base.hpp>
#include <boost/corosio/error.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/capy/results.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/ex/await_context.hpp>
#include <boost/capy/concept/executor.hpp>

#include <system_error>

#include <concepts>
#include <boost/capy/continuation.hpp>

#include <coroutine>
#include <cstddef>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace boost::corosio {

/** Accepts inbound TCP connections, from a coroutine.

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
    OS accept APIs via the `io_context` reactor.

    @par Example
    @par !example convenience_construction

    @par Example
    @par !example fine_grained_setup
*/
class BOOST_COROSIO_DECL tcp_acceptor : public io_object
{
    struct wait_awaitable : detail::void_op_base<wait_awaitable>
    {
    private:
        friend tcp_acceptor;

        wait_awaitable(tcp_acceptor& acc, wait_type w) noexcept
            : acc_(acc)
            , w_(w)
        {
        }

        friend detail::void_op_base<wait_awaitable>;

        tcp_acceptor& acc_;
        wait_type w_;

        std::coroutine_handle<>
        dispatch(capy::continuation& cont, capy::executor_ref ex) const
        {
            return acc_.get().wait(cont, ex, w_, token_, &ec_);
        }
    };

    struct accept_awaitable : detail::void_op_base<accept_awaitable>
    {
    private:
        friend tcp_acceptor;
        friend detail::void_op_base<accept_awaitable>;

        tcp_acceptor& acc_;
        tcp_socket& peer_;
        mutable io_object::implementation* peer_impl_ = nullptr;


        accept_awaitable(tcp_acceptor& acc, tcp_socket& peer) noexcept
            : acc_(acc)
            , peer_(peer)
        {
        }

        std::coroutine_handle<>
        dispatch(capy::continuation& cont, capy::executor_ref ex) const
        {
            return acc_.get().accept(
                cont, ex, this->token_, &this->ec_, &peer_impl_);
        }

    public:
        // A peer accepted for an awaiter that never resumed is closed
        // here, or nothing would own it.
        ~accept_awaitable()
        {
            if (peer_impl_)
                discard_peer(acc_, peer_impl_);
        }

        [[nodiscard]] capy::results<> await_resume() const noexcept
        {
            if (!this->ec_ && peer_impl_)
                peer_.h_.reset(std::exchange(peer_impl_, nullptr));
            return {this->ec_};
        }
    };

    struct accept_value_awaitable : detail::void_op_base<accept_value_awaitable>
    {
    private:
        friend tcp_acceptor;
        friend detail::void_op_base<accept_value_awaitable>;

        tcp_acceptor& acc_;
        mutable io_object::implementation* peer_impl_ = nullptr;


        explicit accept_value_awaitable(tcp_acceptor& acc) noexcept : acc_(acc)
        {
        }

        std::coroutine_handle<>
        dispatch(capy::continuation& cont, capy::executor_ref ex) const
        {
            return acc_.get().accept(
                cont, ex, this->token_, &this->ec_, &peer_impl_);
        }

    public:
        // A peer accepted for an awaiter that never resumed is closed
        // here, or nothing would own it.
        ~accept_value_awaitable()
        {
            if (peer_impl_)
                discard_peer(acc_, peer_impl_);
        }

        [[nodiscard]] capy::results<tcp_socket> await_resume() noexcept
        {
            return take_peer(acc_, this->ec_, peer_impl_);
        }
    };

public:
    /** Closes the acceptor if open, cancelling any pending operations.
    */
    ~tcp_acceptor() override;

    /** Construct an acceptor from an execution context.

        @param ctx The execution context that owns this acceptor.
    */
    explicit tcp_acceptor(capy::execution_context& ctx);

    /** Convenience constructor: open + configure + bind + listen.

        Creates a fully bound listening acceptor in a single
        expression, throwing the codes the piecewise `open()` +
        `set_option()` + `bind()` + `listen()` path reports. The
        address family is deduced from @p ep.

        Before binding, the constructor configures address reuse so a
        server can rebind its port immediately after a restart. It
        sets `SO_REUSEADDR` on POSIX and `SO_EXCLUSIVEADDRUSE` on
        Windows. Windows does not use `SO_REUSEADDR` because it
        instead grants other sockets bind-over rights. A second
        listener on an occupied endpoint therefore throws
        `errc::address_in_use` on every platform.

        @param ctx The execution context that owns this acceptor.
        @param ep The local endpoint to bind to.
        @param backlog The maximum pending connection queue length.

        @throws std::system_error on open, configuration, bind, or
            listen failure.
    */
    tcp_acceptor(capy::execution_context& ctx, endpoint ep, int backlog = 128);

    /** Construct an acceptor from an executor.

        The acceptor is associated with the executor's context. `Ex`
        must satisfy `capy::Executor`.

        @param ex The executor whose context owns the acceptor.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, tcp_acceptor>) &&
        capy::Executor<Ex>
    explicit tcp_acceptor(Ex const& ex) : tcp_acceptor(ex.context())
    {
    }

    /** Convenience constructor from an executor.

        Creates a fully bound listening acceptor in a single
        expression, throwing the codes the piecewise `open()` +
        `set_option()` + `bind()` + `listen()` path reports. The
        address family is deduced from @p ep.

        Before binding, the constructor configures address reuse so a
        server can rebind its port immediately after a restart. It
        sets `SO_REUSEADDR` on POSIX and `SO_EXCLUSIVEADDRUSE` on
        Windows. Windows does not use `SO_REUSEADDR` because it
        instead grants other sockets bind-over rights. A second
        listener on an occupied endpoint therefore throws
        `errc::address_in_use` on every platform.

        `Ex` must satisfy `capy::Executor`.

        @param ex The executor whose context owns the acceptor.
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

    /** Transfers ownership of the acceptor resources.

        @param other The acceptor to move from.

        @pre No awaitables returned by @p other's methods exist.
        @pre The execution context associated with @p other must
            outlive this acceptor.
    */
    tcp_acceptor(tcp_acceptor&& other) noexcept : io_object(std::move(other)) {}

    /** Closes any existing acceptor and transfers ownership.

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

    /// Copy construction is disabled; the handle is uniquely owned.
    tcp_acceptor(tcp_acceptor const&) = delete;
    /// Copy assignment is disabled; the handle is uniquely owned.
    tcp_acceptor& operator=(tcp_acceptor const&) = delete;

    /** Create the acceptor socket without binding or listening.

        Creates a TCP socket with dual-stack enabled for IPv6.
        Does not set SO_REUSEADDR. Call `set_option` explicitly
        if needed.

        If the acceptor is already open, this function is a no-op.

        Failures such as descriptor exhaustion are normal runtime
        conditions and are reported through the returned error code.

        @param f The address family (IPv4 or IPv6). Defaults to
            `family::v4`.

        @par Example
        @par !example open

        @see @ref bind, @ref listen

        @return The error code, empty on success.
    */
    [[nodiscard]] std::error_code open(family f = family::v4) noexcept;

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
        @li `errc::bad_file_descriptor`: The acceptor is not open.
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
            existing connection on this socket is closed.

        @return An awaitable that completes with `results<>`.
            Returns success on successful accept, or an error code on
            failure including:
            - `operation_canceled`: Cancelled via stop_token or cancel().
                Check `ec == cond::canceled` for portable comparison.

        A closed acceptor completes with `errc::bad_file_descriptor`.

        @pre The peer socket must be associated with the same execution context.

        Both this acceptor and @p peer must outlive the returned
        awaitable.

        @par Example
        @par !example accept_into_a_reused_socket

        @see @ref accept()
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

        The caller does not pre-construct the peer socket. The returned
        socket shares this acceptor's execution context.

        The operation supports cancellation via `std::stop_token` through
        the affine awaitable protocol. If the associated stop token is
        triggered, the operation completes immediately with
        `errc::operation_canceled`.

        @return An awaitable that completes with `results<tcp_socket>`.
            On success the payload is the connected peer socket; on failure
            (including cancellation) the error code is set and the payload
            socket is unconnected. Errors include:
            - `operation_canceled`: Cancelled via stop_token or cancel().
                Check `ec == cond::canceled` for portable comparison.

        A closed acceptor completes with `errc::bad_file_descriptor`.
        On failure the returned socket is default-constructed and not
        open. It has no context, so it may only be destroyed, assigned,
        or checked with `is_open()`.

        @pre This acceptor must outlive the returned awaitable.

        @par Example
        @par !example accept_returning_a_new_socket

        @see @ref accept(tcp_socket&)
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
        subsequent @ref accept succeeds without blocking. A
        connection already queued when the wait begins completes
        it immediately. No connection is consumed.

        @note `wait_type::write` is not usable on an acceptor:
        writability carries no meaning for a listening socket, so
        the wait fails with `errc::operation_not_supported` on
        every backend.

        @param w The wait direction.

        @return An awaitable that completes with `results<>`.

        A closed acceptor completes with `errc::bad_file_descriptor`.

        @pre This acceptor must outlive the returned awaitable.
    */
    [[nodiscard]] auto wait(wait_type w)
    {
        wait_awaitable aw(*this, w);
        if (!is_open())
            aw.ec_ = make_error_code(std::errc::bad_file_descriptor);
        return aw;
    }

    /** Cancel any pending asynchronous operations.

        Accept and wait transfer no bytes, so a cancellation always wins:
        an operation reports `errc::operation_canceled` even when it had
        already succeeded when the cancellation landed. Check
        `ec == cond::canceled` for portable comparison.
    */
    void cancel() noexcept;

    /** Get the native socket handle.

        Returns the underlying platform-specific socket descriptor.
        On POSIX systems this is an `int` file descriptor.
        On Windows this is a `SOCKET` handle.

        @return The native socket handle, or -1/INVALID_SOCKET if not open.

        @pre None. May be called on closed acceptors.
    */
    native_handle_type native_handle() const noexcept;

    /** Assign an existing native socket to this acceptor.

        Adopts a listening socket created outside the library. The
        socket may come from a service manager, be inherited, or be
        created natively. Adoption registers the socket with the
        backend. The socket must be a listening stream socket in the
        `AF_INET` or `AF_INET6` family.
        Adoption never alters the descriptor's flags or options: on
        POSIX the fd must already be non-blocking, and on Windows the
        socket must be overlapped-capable.

        Adoption does not verify listen state; @ref accept reports the
        error if the socket is not listening.

        The object must be closed. To replace a held socket, `close()`
        or `release()` it first.

        @par Exception Safety
        Throws nothing. On failure the object is unchanged and the
        caller retains ownership of `fd`.

        @param fd The native socket to adopt. On success the object
            owns it and closes it.

        @return `error::already_open` if this object is open.
            Otherwise the error code, empty on success. Validation and
            registration failures are normal runtime conditions when
            adopting foreign descriptors.
    */
    [[nodiscard]] std::error_code assign(native_handle_type fd) noexcept;

    /** Release ownership of the native socket handle.

        Deregisters the socket from the backend and cancels pending
        operations without closing the descriptor. The caller takes
        ownership of the returned handle.

        A closed object reports `errc::bad_file_descriptor` and the
        returned handle is the invalid sentinel ( -1, or
        `INVALID_SOCKET` on Windows ), never zero.

        @note With the IOCP backend, a socket released while operations
        on it are in flight stays bound to this context's completion
        port for good. Windows refuses to unbind a handle with I/O
        outstanding, so no context, this one included, can assign it
        afterwards. Its cancelled operations still complete normally.

        @return The error code and the native handle.

        @post is_open() == false
    */
    [[nodiscard]] capy::results<native_handle_type> release() noexcept;

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

        Fails with `errc::bad_file_descriptor` if the acceptor is not
        open, or with the error the operating system reports.

        @par Example
        @par !example set_option

        @param opt The option to set.

        @return The error code, empty on success.
    */
    template<class Option>
    [[nodiscard]] std::error_code set_option(Option const& opt) noexcept
    {
        if (!is_open())
            return make_error_code(std::errc::bad_file_descriptor);
        auto const fam = get().family();
        return get().set_option(
            opt.level(fam), opt.name(fam), opt.data(fam), opt.size(fam));
    }

    /** Get a socket option from the acceptor.

        Retrieves the current value of a type-safe socket option.

        Fails with `errc::bad_file_descriptor` if the acceptor is not
        open, or with the error the operating system reports. On
        failure the option is value-initialized.

        @par Example
        @par !example get_option

        @return The error code and the option value.
    */
    template<class Option>
    [[nodiscard]] capy::results<Option> get_option() const noexcept
    {
        if (!is_open())
            return {make_error_code(std::errc::bad_file_descriptor), Option{}};
        Option opt{};
        auto const fam = get().family();
        std::size_t sz = opt.size(fam);
        if (auto ec = get().get_option(
                opt.level(fam), opt.name(fam), opt.data(fam), &sz))
            return {ec, Option{}};
        opt.resize(fam, sz);
        return {std::error_code{}, std::move(opt)};
    }

    /** Define backend hooks for TCP acceptor operations.

        Platform backends derive from this to implement
        accept, endpoint query, open-state checks, cancellation,
        and socket-option management.
    */
    struct implementation : io_object::implementation
    {
        /** Initiate an asynchronous accept operation.

            @param cont Continuation to resume on completion; it lives in
                the awaiting frame until then.
            @param ex Executor for dispatching the completion.
            @param token Stop token for cancellation.
            @param ec Output error code.
            @param impl_out Output implementation for the accepted peer.

            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> accept(
            capy::continuation& cont,
            capy::executor_ref ex,
            std::stop_token token,
            std::error_code* ec,
            io_object::implementation** impl_out) = 0;

        /** Initiate an asynchronous wait for acceptor readiness.

            Completes when the listen socket becomes ready for
            the specified direction (typically `wait_type::read`
            for an incoming connection), or an error condition is
            reported. No connection is consumed.

            @param cont Continuation to resume on completion; it lives in
                the awaiting frame until then.
            @param ex Executor for dispatching the completion.
            @param w The direction to wait on.
            @param token Stop token for cancellation.
            @param ec Output error code.

            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> wait(
            capy::continuation& cont,
            capy::executor_ref ex,
            wait_type w,
            std::stop_token token,
            std::error_code* ec) = 0;

        /** Returns the cached local endpoint.

            @return The cached local endpoint.
        */
        virtual endpoint local_endpoint() const noexcept = 0;

        /** Return true if the acceptor has a kernel resource open.

            @return true if the acceptor has a kernel resource open.
        */
        virtual bool is_open() const noexcept = 0;

        /** Return the native handle, or the platform sentinel if closed.

            @return The native handle, or the platform sentinel if closed.
        */
        virtual native_handle_type native_handle() const noexcept = 0;

        /** Return the socket's address family.

            Socket options render for this family.

            @return The socket's address family.
        */
        virtual corosio::family family() const noexcept = 0;

        /** Release and return the native handle without closing.

            @return The native handle.
        */
        virtual native_handle_type release_socket() noexcept = 0;

        /** Cancel any pending asynchronous operations.

            Accept and wait transfer no bytes, so a cancellation always
            wins: an operation reports `operation_canceled` even when it
            had already succeeded when the cancellation landed.
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
    /** Adopt an existing handle.

        @param h The handle the acceptor takes ownership of.
    */
    explicit tcp_acceptor(handle h) noexcept : io_object(std::move(h)) {}

    /** Transfer the accepted peer implementation to the peer socket.

        @param peer The socket that receives the transferred implementation.
        @param impl The accepted peer implementation, or null to do nothing.
    */
    static void
    reset_peer_impl(tcp_socket& peer, io_object::implementation* impl) noexcept
    {
        if (impl)
            peer.h_.reset(impl);
    }

    /** Return the result of an accept that yields the peer.

        The peer socket is built only on success, so a failed accept
        never touches the acceptor's context, which a moved-from
        acceptor lacks. On failure the peer is default-constructed.

        @param acc The acceptor that performed the accept.
        @param ec The completion code.
        @param impl The accepted peer implementation; taken on success.

        @return The completion code and the peer.
    */
    static capy::results<tcp_socket> take_peer(
        tcp_acceptor& acc,
        std::error_code ec,
        io_object::implementation*& impl) noexcept
    {
        if (ec || !impl)
            return {ec, tcp_socket()};

        tcp_socket peer(acc.context());
        peer.h_.reset(std::exchange(impl, nullptr));
        return {ec, std::move(peer)};
    }

    /// Close and release a non-null accepted peer no socket took over.
    static void
    discard_peer(tcp_acceptor& acc, io_object::implementation* impl) noexcept;

private:
    inline implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif
