//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_ACCEPTOR_HPP
#define BOOST_COROSIO_ACCEPTOR_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/io_object.hpp>
#include <boost/corosio/io_result.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/socket.hpp>
#include <boost/capy/ex/any_dispatcher.hpp>
#include <boost/capy/concept/affine_awaitable.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/concept/executor.hpp>

#include <boost/system/error_code.hpp>

#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <stop_token>
#include <type_traits>

namespace boost {
namespace corosio {

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

    @par Example
    @code
    io_context ioc;
    acceptor acc(ioc);
    acc.listen(endpoint(8080));  // Bind to port 8080

    socket peer(ioc);
    auto [ec] = co_await acc.accept(peer);
    if (!ec) {
        // peer is now a connected socket
        auto [ec2, n] = co_await peer.read_some(buf);
    }
    @endcode
*/
class acceptor : public io_object
{
    struct accept_awaitable
    {
        acceptor& acc_;
        socket& peer_;
        std::stop_token token_;
        mutable system::error_code ec_;
        mutable io_object::io_object_impl* peer_impl_ = nullptr;

        accept_awaitable(acceptor& acc, socket& peer) noexcept
            : acc_(acc)
            , peer_(peer)
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        io_result<> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(system::errc::operation_canceled)};
            
            // Transfer the accepted impl to the peer socket
            // (acceptor is a friend of socket, so we can access impl_)
            if (!ec_ && peer_impl_)
            {
                peer_.close();
                peer_.impl_ = peer_impl_;
            }
            return {ec_};
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d) -> std::coroutine_handle<>
        {
            acc_.get().accept(h, d, token_, &ec_, &peer_impl_);
            return std::noop_coroutine();
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d,
            std::stop_token token) -> std::coroutine_handle<>
        {
            token_ = std::move(token);
            acc_.get().accept(h, d, token_, &ec_, &peer_impl_);
            return std::noop_coroutine();
        }
    };

public:
    /** Destructor.

        Closes the acceptor if open, cancelling any pending operations.
    */
    BOOST_COROSIO_DECL
    ~acceptor();

    /** Construct an acceptor from an execution context.

        @param ctx The execution context that will own this acceptor.
    */
    BOOST_COROSIO_DECL
    explicit acceptor(capy::execution_context& ctx);

    /** Construct an acceptor from an executor.

        The acceptor is associated with the executor's context.

        @param ex The executor whose context will own the acceptor.
    */
    template<class Executor>
        requires (!std::same_as<std::remove_cvref_t<Executor>, acceptor>) &&
                 capy::executor<Executor>
    explicit acceptor(Executor const& ex)
        : acceptor(ex.context())
    {
    }

    /** Move constructor.

        Transfers ownership of the acceptor resources.

        @param other The acceptor to move from.
    */
    acceptor(acceptor&& other) noexcept
        : io_object(other.context())
    {
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }

    /** Move assignment operator.

        Closes any existing acceptor and transfers ownership.
        The source and destination must share the same execution context.

        @param other The acceptor to move from.

        @return Reference to this acceptor.

        @throws std::logic_error if the acceptors have different execution contexts.
    */
    acceptor& operator=(acceptor&& other)
    {
        if (this != &other)
        {
            if (ctx_ != other.ctx_)
                detail::throw_logic_error(
                    "cannot move acceptor across execution contexts");
            close();
            impl_ = other.impl_;
            other.impl_ = nullptr;
        }
        return *this;
    }

    acceptor(acceptor const&) = delete;
    acceptor& operator=(acceptor const&) = delete;

    /** Open, bind, and listen on an endpoint.

        Creates an IPv4 TCP socket, binds it to the specified endpoint,
        and begins listening for incoming connections. This must be
        called before initiating accept operations.

        @param ep The local endpoint to bind to. Use `endpoint(port)` to
            bind to all interfaces on a specific port.

        @param backlog The maximum length of the queue of pending
            connections. Defaults to a reasonable system value.

        @throws std::system_error on failure.
    */
    BOOST_COROSIO_DECL
    void listen(endpoint ep, int backlog = 128);

    /** Close the acceptor.

        Releases acceptor resources. Any pending operations complete
        with `errc::operation_canceled`.
    */
    BOOST_COROSIO_DECL
    void close();

    /** Check if the acceptor is listening.

        @return `true` if the acceptor is open and listening.
    */
    bool is_open() const noexcept
    {
        return impl_ != nullptr;
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
            - operation_canceled: Cancelled via stop_token or cancel()

        @par Preconditions
        The acceptor must be listening (`is_open() == true`).
        The peer socket must be associated with the same execution context.

        @par Example
        @code
        socket peer(ioc);
        auto [ec] = co_await acc.accept(peer);
        if (!ec) {
            // Use peer socket
        }
        @endcode
    */
    auto accept(socket& peer)
    {
        assert(impl_ != nullptr);
        return accept_awaitable(*this, peer);
    }

    /** Cancel any pending asynchronous operations.

        All outstanding operations complete with `errc::operation_canceled`.
    */
    BOOST_COROSIO_DECL
    void cancel();

    struct acceptor_impl : io_object_impl
    {
        virtual void accept(
            std::coroutine_handle<>,
            capy::any_dispatcher,
            std::stop_token,
            system::error_code*,
            io_object_impl**) = 0;
    };

private:

    inline acceptor_impl& get() const noexcept
    {
        return *static_cast<acceptor_impl*>(impl_);
    }
};

} // namespace corosio
} // namespace boost

#endif
