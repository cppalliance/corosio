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

/** Options for @ref local_stream_acceptor::bind. */
enum class bind_option
{
    /// Default: do not unlink the socket path.
    none,
    /// Unlink the socket path before binding (ignored for abstract paths).
    unlink_existing
};

/** An asynchronous Unix stream acceptor for coroutine I/O.

    This class provides asynchronous Unix domain stream accept
    operations that return awaitable types. The acceptor binds
    to a local endpoint (filesystem path) and listens for
    incoming connections.

    The library does NOT automatically unlink the socket path
    on close. Callers are responsible for removing the socket
    file before bind or after close, or pass
    @ref bind_option::unlink_existing to @ref bind.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. An acceptor must not have concurrent
    accept operations.

    @par Semantics
    Wraps the platform Unix domain socket listener. Operations
    dispatch to OS accept APIs via the io_context reactor.
    Cancellation propagates through the IoAwaitable stop_token
    or via @ref cancel; cancelled operations resume with
    `errc::operation_canceled`.

    @par Example
    @code
    io_context ioc;
    local_stream_acceptor acc(ioc);
    acc.open();
    if (auto ec = acc.bind(
            local_endpoint("/tmp/my.sock"),
            bind_option::unlink_existing))
        return ec;
    if (auto ec = acc.listen())
        return ec;

    local_stream_socket peer(ioc);
    auto [ec] = co_await acc.accept(peer);
    if (!ec) {
        // peer is now connected
    }
    @endcode

    @see local_stream_socket, local_endpoint, local_stream
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
            if (token_.stop_requested())
            {
                ec_ = make_error_code(std::errc::operation_canceled);
                return h;
            }
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
            if (token_.stop_requested())
            {
                ec_ = make_error_code(std::errc::operation_canceled);
                return h;
            }
            return acc_.get().accept(
                h, env->executor, token_, &ec_, &peer_impl_);
        }
    };

public:
    ~local_stream_acceptor() override;

    explicit local_stream_acceptor(capy::execution_context& ctx);

    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, local_stream_acceptor>) &&
        capy::Executor<Ex>
    explicit local_stream_acceptor(Ex const& ex) : local_stream_acceptor(ex.context())
    {
    }

    local_stream_acceptor(local_stream_acceptor&& other) noexcept
        : local_stream_acceptor(other.ctx_, std::move(other))
    {
    }

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

    /// Close the acceptor.
    void close();

    /** Check if the acceptor has a native handle.

        Returns true once @ref open succeeds and until @ref close is
        called. This does not indicate that @ref listen has been
        invoked — an open-but-not-listening acceptor will still
        report `true`.
    */
    bool is_open() const noexcept
    {
        return h_ && get().is_open();
    }

    /** Initiate an asynchronous accept operation.

        @param peer The socket to receive the accepted connection.

        @return An awaitable that completes with io_result<>.

        @throws std::logic_error if the native acceptor handle is
            absent (i.e., `!is_open()`). Calling accept on an
            open-but-not-listening acceptor does not throw; the
            awaitable completes with a kernel error such as
            `errc::invalid_argument` (EINVAL).
    */
    auto accept(local_stream_socket& peer)
    {
        if (!is_open())
            detail::throw_logic_error("accept: acceptor not open");
        return accept_awaitable(*this, peer);
    }

    /** Initiate an asynchronous accept, returning the socket.

        @return An awaitable that completes with
            io_result<local_stream_socket>.

        @throws std::logic_error if the native acceptor handle is
            absent (i.e., `!is_open()`). Calling accept on an
            open-but-not-listening acceptor does not throw; the
            awaitable completes with a kernel error such as
            `errc::invalid_argument` (EINVAL).
    */
    auto accept()
    {
        if (!is_open())
            detail::throw_logic_error("accept: acceptor not open");
        return move_accept_awaitable(*this);
    }

    void cancel();

    /** Release ownership of the native socket handle.

        Deregisters the acceptor from the reactor and cancels
        pending operations without closing the fd.

        @return The native handle.

        @throws std::logic_error if the acceptor is not open.
    */
    native_handle_type release();

    corosio::local_endpoint local_endpoint() const noexcept;

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

    /** Define backend hooks for local stream acceptor operations. */
    struct implementation : io_object::implementation
    {
        virtual std::coroutine_handle<> accept(
            std::coroutine_handle<>,
            capy::executor_ref,
            std::stop_token,
            std::error_code*,
            io_object::implementation**) = 0;

        virtual corosio::local_endpoint local_endpoint() const noexcept = 0;

        virtual bool is_open() const noexcept = 0;

        virtual native_handle_type release_socket() noexcept = 0;

        virtual void cancel() noexcept = 0;

        virtual std::error_code set_option(
            int level,
            int optname,
            void const* data,
            std::size_t size) noexcept = 0;

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
