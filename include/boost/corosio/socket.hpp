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
#include <boost/corosio/detail/socket_impl.hpp>
#include <boost/corosio/buffers_param.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/capy/affine.hpp>
#include <boost/capy/execution_context.hpp>
#include <boost/capy/executor.hpp>

#include <boost/system/error_code.hpp>

#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <stop_token>
#include <type_traits>

namespace boost {
namespace corosio {

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

    auto ec = co_await s.connect(
        tcp::endpoint(urls::ipv4_address::loopback(), 8080));
    if (ec)
        co_return;

    char buf[1024];
    auto [read_ec, n] = co_await s.read_some(
        buffers::mutable_buffer(buf, sizeof(buf)));
    @endcode
*/
class socket
{
    struct connect_awaitable
    {
        socket& s_;
        tcp::endpoint endpoint_;
        std::stop_token token_;
        mutable system::error_code ec_;

        connect_awaitable(socket& s, tcp::endpoint ep) noexcept
            : s_(s)
            , endpoint_(ep)
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        system::error_code await_resume() const noexcept
        {
            if (token_.stop_requested())
                return make_error_code(system::errc::operation_canceled);
            return ec_;
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d) -> std::coroutine_handle<>
        {
            s_.impl_->connect(h, d, endpoint_, token_, &ec_);
            return std::noop_coroutine();
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d,
            std::stop_token token) -> std::coroutine_handle<>
        {
            token_ = std::move(token);
            s_.impl_->connect(h, d, endpoint_, token_, &ec_);
            return std::noop_coroutine();
        }
    };

    template<class MutableBufferSequence>
    struct read_some_awaitable
    {
        socket& s_;
        MutableBufferSequence buffers_;
        std::stop_token token_;
        mutable system::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        read_some_awaitable(socket& s, MutableBufferSequence buffers) noexcept
            : s_(s)
            , buffers_(std::move(buffers))
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        std::pair<system::error_code, std::size_t> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(system::errc::operation_canceled), 0};
            return {ec_, bytes_transferred_};
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d) -> std::coroutine_handle<>
        {
            buffers_param_impl param(buffers_);
            s_.impl_->read_some(h, d, param, token_, &ec_, &bytes_transferred_);
            return std::noop_coroutine();
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d,
            std::stop_token token) -> std::coroutine_handle<>
        {
            token_ = std::move(token);
            buffers_param_impl param(buffers_);
            s_.impl_->read_some(h, d, param, token_, &ec_, &bytes_transferred_);
            return std::noop_coroutine();
        }
    };

    template<class ConstBufferSequence>
    struct write_some_awaitable
    {
        socket& s_;
        ConstBufferSequence buffers_;
        std::stop_token token_;
        mutable system::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        write_some_awaitable(socket& s, ConstBufferSequence buffers) noexcept
            : s_(s)
            , buffers_(std::move(buffers))
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        std::pair<system::error_code, std::size_t> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(system::errc::operation_canceled), 0};
            return {ec_, bytes_transferred_};
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d) -> std::coroutine_handle<>
        {
            buffers_param_impl param(buffers_);
            s_.impl_->write_some(h, d, param, token_, &ec_, &bytes_transferred_);
            return std::noop_coroutine();
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d,
            std::stop_token token) -> std::coroutine_handle<>
        {
            token_ = std::move(token);
            buffers_param_impl param(buffers_);
            s_.impl_->write_some(h, d, param, token_, &ec_, &bytes_transferred_);
            return std::noop_coroutine();
        }
    };

public:
    /** Destructor.

        Closes the socket if open, cancelling any pending operations.
    */
    BOOST_COROSIO_DECL
    ~socket();

    /** Construct a socket from an execution context.

        @param ctx The execution context that will own this socket.
    */
    BOOST_COROSIO_DECL
    explicit socket(capy::execution_context& ctx);

    /** Construct a socket from an executor.

        The socket is associated with the executor's context.

        @param ex The executor whose context will own the socket.
    */
    template<class Executor>
        requires (!std::same_as<std::remove_cvref_t<Executor>, socket>) &&
                 capy::executor<Executor>
    explicit socket(Executor const& ex)
        : socket(ex.context())
    {
    }

    /** Move constructor.

        Transfers ownership of the socket resources.

        @param other The socket to move from.
    */
    socket(socket&& other) noexcept
        : ctx_(other.ctx_)
        , impl_(other.impl_)
    {
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
    BOOST_COROSIO_DECL
    void open();

    /** Close the socket.

        Releases socket resources. Any pending operations complete
        with `errc::operation_canceled`.
    */
    BOOST_COROSIO_DECL
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

        @return An awaitable that completes with `system::error_code`.
            Returns success (default error_code) on successful connection,
            or an error code on failure including:
            - connection_refused: No server listening at endpoint
            - timed_out: Connection attempt timed out
            - network_unreachable: No route to host
            - operation_canceled: Cancelled via stop_token or cancel()

        @par Preconditions
        The socket must be open (`is_open() == true`).
    */
    auto connect(tcp::endpoint ep)
    {
        assert(impl_ != nullptr);
        return connect_awaitable(*this, ep);
    }

    /** Initiate an asynchronous read operation.

        Reads available data into the provided buffer sequence. The
        operation completes when at least one byte has been read, or
        an error occurs.

        The operation supports cancellation via `std::stop_token` through
        the affine awaitable protocol. If the associated stop token is
        triggered, the operation completes immediately with
        `errc::operation_canceled`.

        @param buffers The buffer sequence to read data into.

        @return An awaitable that completes with a pair of
            `{error_code, bytes_transferred}`. Returns success with the
            number of bytes read, or an error code on failure including:
            - connection_reset: Peer closed the connection
            - operation_canceled: Cancelled via stop_token or cancel()

        @par Preconditions
        The socket must be open and connected.

        @note This function may return fewer bytes than the buffer
            capacity. Use a loop to read an exact amount.
    */
    template<class MutableBufferSequence>
    auto read_some(MutableBufferSequence const& buffers)
    {
        assert(impl_ != nullptr);
        return read_some_awaitable<MutableBufferSequence>(*this, buffers);
    }

    /** Initiate an asynchronous write operation.

        Writes data from the provided buffer sequence. The operation
        completes when at least one byte has been written, or an
        error occurs.

        The operation supports cancellation via `std::stop_token` through
        the affine awaitable protocol. If the associated stop token is
        triggered, the operation completes immediately with
        `errc::operation_canceled`.

        @param buffers The buffer sequence containing data to write.

        @return An awaitable that completes with a pair of
            `{error_code, bytes_transferred}`. Returns success with the
            number of bytes written, or an error code on failure including:
            - broken_pipe: Connection closed by peer
            - operation_canceled: Cancelled via stop_token or cancel()

        @par Preconditions
        The socket must be open and connected.

        @note This function may write fewer bytes than the buffer
            contains. Use a loop to write all data.
    */
    template<class ConstBufferSequence>
    auto write_some(ConstBufferSequence const& buffers)
    {
        assert(impl_ != nullptr);
        return write_some_awaitable<ConstBufferSequence>(*this, buffers);
    }

    /** Cancel any pending asynchronous operations.

        All outstanding operations complete with `errc::operation_canceled`.
    */
    BOOST_COROSIO_DECL
    void cancel();

    /** Return the execution context.

        @return Reference to the execution context that owns this socket.
    */
    auto
    context() const noexcept ->
        capy::execution_context&
    {
        return *ctx_;
    }

private:
    friend class tcp::acceptor;

    capy::execution_context* ctx_;
    detail::socket_impl* impl_ = nullptr;
};

} // namespace corosio
} // namespace boost

#endif
