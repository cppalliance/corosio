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
#include <boost/corosio/io/io_object.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/concept/executor.hpp>

#include <system_error>

#include <concepts>
#include <coroutine>
#include <cstddef>
#include <memory>
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
    io_context ioc;
    tcp_acceptor acc(ioc);
    if (auto ec = acc.listen(endpoint(8080)))  // Bind to port 8080
        return ec;

    tcp_socket peer(ioc);
    auto [ec] = co_await acc.accept(peer);
    if (!ec) {
        // peer is now a connected socket
        auto [ec2, n] = co_await peer.read_some(buf);
    }
    @endcode
*/
class BOOST_COROSIO_DECL tcp_acceptor : public io_object
{
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
            return token_.stop_requested();
        }

        capy::io_result<> await_resume() const noexcept
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

public:
    /** Destructor.

        Closes the acceptor if open, cancelling any pending operations.
    */
    ~tcp_acceptor() override;

    /** Construct an acceptor from an execution context.

        @param ctx The execution context that will own this acceptor.
    */
    explicit tcp_acceptor(capy::execution_context& ctx);

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

    /** Move constructor.

        Transfers ownership of the acceptor resources.

        @param other The acceptor to move from.
    */
    tcp_acceptor(tcp_acceptor&& other) noexcept : io_object(std::move(other)) {}

    /** Move assignment operator.

        Closes any existing acceptor and transfers ownership.
        @param other The acceptor to move from.

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

    /** Open, bind, and listen on an endpoint.

        Creates an IPv4 TCP socket, binds it to the specified endpoint,
        and begins listening for incoming connections. This must be
        called before initiating accept operations.

        @param ep The local endpoint to bind to. Use `endpoint(port)` to
            bind to all interfaces on a specific port.

        @param backlog The maximum length of the queue of pending
            connections. Defaults to 128.

        @return An error code indicating success or the reason for failure.
            A default-constructed error code indicates success.

        @par Error Conditions
        @li `errc::address_in_use`: The endpoint is already in use.
        @li `errc::address_not_available`: The address is not available
            on any local interface.
        @li `errc::permission_denied`: Insufficient privileges to bind
            to the endpoint (e.g., privileged port).
        @li `errc::operation_not_supported`: The acceptor service is
            unavailable in the context (POSIX only).

        @throws Nothing.
    */
    [[nodiscard]] std::error_code listen(endpoint ep, int backlog = 128);

    /** Close the acceptor.

        Releases acceptor resources. Any pending operations complete
        with `errc::operation_canceled`.
    */
    void close();

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

        @par Preconditions
        The acceptor must be listening (`is_open() == true`).
        The peer socket must be associated with the same execution context.

        @par Example
        @code
        tcp_socket peer(ioc);
        auto [ec] = co_await acc.accept(peer);
        if (!ec) {
            // Use peer socket
        }
        @endcode
    */
    auto accept(tcp_socket& peer)
    {
        if (!is_open())
            detail::throw_logic_error("accept: acceptor not listening");
        return accept_awaitable(*this, peer);
    }

    /** Cancel any pending asynchronous operations.

        All outstanding operations complete with `errc::operation_canceled`.
        Check `ec == cond::canceled` for portable comparison.
    */
    void cancel();

    /** Get the local endpoint of the acceptor.

        Returns the local address and port to which the acceptor is bound.
        This is useful when binding to port 0 (ephemeral port) to discover
        the OS-assigned port number. The endpoint is cached when listen()
        is called.

        @return The local endpoint, or a default endpoint (0.0.0.0:0) if
            the acceptor is not listening.

        @par Thread Safety
        The cached endpoint value is set during listen() and cleared
        during close(). This function may be called concurrently with
        accept operations, but must not be called concurrently with
        listen() or close().
    */
    endpoint local_endpoint() const noexcept;

    struct implementation : io_object::implementation
    {
        virtual std::coroutine_handle<> accept(
            std::coroutine_handle<>,
            capy::executor_ref,
            std::stop_token,
            std::error_code*,
            io_object::implementation**) = 0;

        /// Returns the cached local endpoint.
        virtual endpoint local_endpoint() const noexcept = 0;

        /// Return true if the acceptor has a kernel resource open.
        virtual bool is_open() const noexcept = 0;

        /** Cancel any pending asynchronous operations.

            All outstanding operations complete with operation_canceled error.
        */
        virtual void cancel() noexcept = 0;
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
