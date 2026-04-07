//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_LOCAL_STREAM_ACCEPTOR_HPP
#define BOOST_COROSIO_LOCAL_STREAM_ACCEPTOR_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/concept/executor.hpp>

#include <system_error>

#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <stop_token>
#include <type_traits>

namespace boost::corosio {

/** Options for @ref local_stream_acceptor::bind().

    Controls filesystem cleanup behavior before binding
    to a Unix domain socket path.
*/
enum class bind_option
{
    none,
    /// Unlink the socket path before binding (ignored for abstract paths).
    unlink_existing
};

/** An asynchronous Unix domain stream acceptor for coroutine I/O.

    This class provides asynchronous Unix domain stream accept
    operations that return awaitable types. The acceptor binds
    to a local endpoint (filesystem path or abstract name) and
    listens for incoming connections.

    The library does NOT automatically unlink the socket path
    on close. Callers are responsible for removing the socket
    file before bind (via @ref bind_option::unlink_existing) or
    after close.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. An acceptor must not have concurrent
    accept operations.

    @par Example
    @code
    io_context ioc;
    local_stream_acceptor acc(ioc);
    acc.open();
    acc.bind(local_endpoint("/tmp/my.sock"),
             bind_option::unlink_existing);
    acc.listen();
    auto [ec, peer] = co_await acc.accept();
    @endcode
*/
class BOOST_COROSIO_DECL local_stream_acceptor : public io_object
{
    struct move_accept_awaitable
    {
        local_stream_acceptor& acc_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable io_object::implementation* peer_impl_ = nullptr;

        explicit move_accept_awaitable(
            local_stream_acceptor& acc) noexcept
            : acc_(acc)
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        capy::io_result<local_stream_socket> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(std::errc::operation_canceled),
                        local_stream_socket()};

            if (ec_ || !peer_impl_)
                return {ec_, local_stream_socket()};

            local_stream_socket peer(acc_.ctx_);
            reset_peer_impl(peer, peer_impl_);
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

    struct accept_awaitable
    {
        local_stream_acceptor& acc_;
        local_stream_socket& peer_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable io_object::implementation* peer_impl_ = nullptr;

        accept_awaitable(
            local_stream_acceptor& acc, local_stream_socket& peer) noexcept
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
    ~local_stream_acceptor() override;

    /** Construct an acceptor from an execution context.

        @param ctx The execution context that will own this acceptor.
    */
    explicit local_stream_acceptor(capy::execution_context& ctx);

    /** Construct an acceptor from an executor.

        The acceptor is associated with the executor's context.

        @param ex The executor whose context will own the acceptor.

        @tparam Ex A type satisfying @ref capy::Executor. Must not
            be `local_stream_acceptor` itself (disables implicit
            conversion from move).
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, local_stream_acceptor>) &&
        capy::Executor<Ex>
    explicit local_stream_acceptor(Ex const& ex) : local_stream_acceptor(ex.context())
    {
    }

    /** Move constructor.

        Transfers ownership of the acceptor resources.

        @param other The acceptor to move from.

        @pre No awaitables returned by @p other's methods exist.
        @pre The execution context associated with @p other must
            outlive this acceptor.
    */
    local_stream_acceptor(local_stream_acceptor&& other) noexcept
        : local_stream_acceptor(other.ctx_, std::move(other))
    {
    }

    /** Move assignment operator.

        Closes any existing acceptor and transfers ownership.
        Both acceptors must share the same execution context.

        @param other The acceptor to move from.

        @return Reference to this acceptor.

        @pre `&ctx_ == &other.ctx_` (same execution context).
        @pre No awaitables returned by either `*this` or @p other's
            methods exist.
    */
    local_stream_acceptor& operator=(local_stream_acceptor&& other) noexcept
    {
        assert(&ctx_ == &other.ctx_ &&
            "move-assign requires the same execution_context");
        if (this != &other)
        {
            close();
            io_object::operator=(std::move(other));
        }
        return *this;
    }

    local_stream_acceptor(local_stream_acceptor const&)            = delete;
    local_stream_acceptor& operator=(local_stream_acceptor const&) = delete;

    /** Create the acceptor socket.

        @param proto The protocol. Defaults to local_stream{}.

        @throws std::system_error on failure.
    */
    void open(local_stream proto = {});

    /** Bind to a local endpoint.

        @param ep The local endpoint (path) to bind to.
        @param opt Bind options. Pass bind_option::unlink_existing
            to unlink the socket path before binding (ignored for
            abstract sockets and empty endpoints).

        @return An error code on failure, empty on success.

        @throws std::logic_error if the acceptor is not open.
    */
    [[nodiscard]] std::error_code
    bind(corosio::local_endpoint ep,
         bind_option opt = bind_option::none);

    /** Start listening for incoming connections.

        @param backlog The maximum pending connection queue length.

        @return An error code on failure, empty on success.

        @throws std::logic_error if the acceptor is not open.
    */
    [[nodiscard]] std::error_code listen(int backlog = 128);

    /** Close the acceptor.

        Cancels any pending accept operations and releases the
        underlying socket. Has no effect if the acceptor is not
        open.

        @post is_open() == false
    */
    void close();

    /// Check if the acceptor has an open socket handle.
    bool is_open() const noexcept
    {
        return h_ && get().is_open();
    }

    /** Initiate an asynchronous accept into an existing socket.

        Completes when a new connection is available. On success
        @p peer is reset to the accepted connection. Only one
        accept may be in flight at a time.

        @param peer The socket to receive the accepted connection.

        @par Cancellation
        Supports cancellation via stop_token or cancel().
        On cancellation, yields `capy::cond::canceled` and
        @p peer is not modified.

        @return An awaitable that completes with io_result<>.

        @throws std::logic_error if the acceptor is not open.
    */
    auto accept(local_stream_socket& peer)
    {
        if (!is_open())
            detail::throw_logic_error("accept: acceptor not listening");
        return accept_awaitable(*this, peer);
    }

    /** Initiate an asynchronous accept, returning the socket.

        Completes when a new connection is available. Only one
        accept may be in flight at a time.

        @par Cancellation
        Supports cancellation via stop_token or cancel().
        On cancellation, yields `capy::cond::canceled` with
        a default-constructed socket.

        @return An awaitable that completes with
            io_result<local_stream_socket>.

        @throws std::logic_error if the acceptor is not open.
    */
    auto accept()
    {
        if (!is_open())
            detail::throw_logic_error("accept: acceptor not listening");
        return move_accept_awaitable(*this);
    }

    /** Cancel pending asynchronous accept operations.

        Outstanding accept operations complete with
        @c capy::cond::canceled. Safe to call when no
        operations are pending (no-op).
    */
    void cancel();

    /** Release ownership of the native socket handle.

        Deregisters the acceptor from the reactor and cancels
        pending operations without closing the descriptor. The
        caller takes ownership of the returned handle.

        @return The native handle.

        @throws std::logic_error if the acceptor is not open.

        @post is_open() == false
    */
    native_handle_type release();

    /** Return the local endpoint the acceptor is bound to.

        Returns a default-constructed (empty) endpoint if the
        acceptor is not open or not yet bound. Safe to call in
        any state.
    */
    corosio::local_endpoint local_endpoint() const noexcept;

    /** Set a socket option on the acceptor.

        Applies a type-safe socket option to the underlying socket.
        The option type encodes the protocol level and option name.

        @param opt The option to set.

        @tparam Option A socket option type providing static
            `level()` and `name()` members, and `data()` / `size()`
            accessors.

        @throws std::logic_error if the acceptor is not open.
        @throws std::system_error on failure.
    */
    template<class Option>
    void set_option(Option const& opt)
    {
        if (!is_open())
            detail::throw_logic_error("set_option: acceptor not open");
        std::error_code ec = get().set_option(
            Option::level(), Option::name(), opt.data(), opt.size());
        if (ec)
            detail::throw_system_error(ec, "local_stream_acceptor::set_option");
    }

    /** Get a socket option from the acceptor.

        Retrieves the current value of a type-safe socket option.

        @return The current option value.

        @tparam Option A socket option type providing static
            `level()` and `name()` members, and `data()` / `size()`
            / `resize()` members.

        @throws std::logic_error if the acceptor is not open.
        @throws std::system_error on failure.
    */
    template<class Option>
    Option get_option() const
    {
        if (!is_open())
            detail::throw_logic_error("get_option: acceptor not open");
        Option opt{};
        std::size_t sz = opt.size();
        std::error_code ec =
            get().get_option(Option::level(), Option::name(), opt.data(), &sz);
        if (ec)
            detail::throw_system_error(ec, "local_stream_acceptor::get_option");
        opt.resize(sz);
        return opt;
    }

    /** Backend hooks for local stream acceptor operations.

        Platform backends derive from this to implement
        accept, option, and lifecycle management.
    */
    struct implementation : io_object::implementation
    {
        /** Initiate an asynchronous accept.

            On completion the backend sets @p *ec and, on
            success, stores a pointer to the new socket
            implementation in @p *impl_out.

            @param h Coroutine handle to resume.
            @param ex Executor for dispatching the completion.
            @param token Stop token for cancellation.
            @param ec Output error code.
            @param impl_out Output pointer for the accepted socket.
            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> accept(
            std::coroutine_handle<>,
            capy::executor_ref,
            std::stop_token,
            std::error_code*,
            io_object::implementation**) = 0;

        /// Return the cached local endpoint.
        virtual corosio::local_endpoint local_endpoint() const noexcept = 0;

        /// Return whether the underlying socket is open.
        virtual bool is_open() const noexcept = 0;

        /// Release and return the native handle without closing.
        virtual native_handle_type release_socket() noexcept = 0;

        /// Cancel pending accept operations.
        virtual void cancel() noexcept = 0;

        /// Set a raw socket option.
        virtual std::error_code set_option(
            int level,
            int optname,
            void const* data,
            std::size_t size) noexcept = 0;

        /// Get a raw socket option.
        virtual std::error_code
        get_option(int level, int optname, void* data, std::size_t* size)
            const noexcept = 0;
    };

protected:
    local_stream_acceptor(handle h, capy::execution_context& ctx) noexcept
        : io_object(std::move(h))
        , ctx_(ctx)
    {
    }

    local_stream_acceptor(
        capy::execution_context& ctx, local_stream_acceptor&& other) noexcept
        : io_object(std::move(other))
        , ctx_(ctx)
    {
    }

    static void reset_peer_impl(
        local_stream_socket& peer, io_object::implementation* impl) noexcept
    {
        if (impl)
            peer.h_.reset(impl);
    }

private:
    capy::execution_context& ctx_;

    inline implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_LOCAL_STREAM_ACCEPTOR_HPP
