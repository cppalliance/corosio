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
    Operations support cancellation through std::stop_token via
    the affine protocol, or explicitly through cancel().

    Satisfies capy::Stream.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. A socket must not have concurrent
    operations of the same type. One read and one write may be
    in flight simultaneously.
*/
class BOOST_COROSIO_DECL local_stream_socket : public io_stream
{
public:
    using shutdown_type = corosio::shutdown_type;
    using enum corosio::shutdown_type;

    /** Define backend hooks for local stream socket operations.

        Platform backends (epoll, kqueue, select) derive from this
        to implement socket I/O, connection, and option management.
    */
    struct implementation : io_stream::implementation
    {
        virtual std::coroutine_handle<> connect(
            std::coroutine_handle<> h,
            capy::executor_ref ex,
            corosio::local_endpoint ep,
            std::stop_token token,
            std::error_code* ec) = 0;

        virtual std::error_code shutdown(shutdown_type what) noexcept = 0;

        virtual native_handle_type native_handle() const noexcept = 0;

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

        virtual corosio::local_endpoint local_endpoint() const noexcept = 0;

        virtual corosio::local_endpoint remote_endpoint() const noexcept = 0;
    };

    /// Represent the awaitable returned by connect.
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

public:
    ~local_stream_socket() override;

    explicit local_stream_socket(capy::execution_context& ctx);

    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, local_stream_socket>) &&
        capy::Executor<Ex>
    explicit local_stream_socket(Ex const& ex) : local_stream_socket(ex.context())
    {
    }

    local_stream_socket(local_stream_socket&& other) noexcept
        : io_object(std::move(other))
    {
    }

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

    /// Close the socket.
    void close();

    /// Check if the socket is open.
    bool is_open() const noexcept
    {
        return h_ && get().native_handle() >= 0;
    }

    /** Initiate an asynchronous connect operation.

        If the socket is not already open, it is opened automatically.

        @param ep The peer endpoint (path) to connect to.

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

    void cancel();

    native_handle_type native_handle() const noexcept;

    /** Query the number of bytes available for reading.

        @return The number of bytes that can be read without blocking.

        @throws std::logic_error if the socket is not open.
        @throws std::system_error on ioctl failure.
    */
    std::size_t available() const;

    /** Release ownership of the native socket handle.

        Deregisters the socket from the reactor and cancels pending
        operations without closing the fd. The caller takes ownership
        of the returned descriptor.

        @return The native handle.

        @throws std::logic_error if the socket is not open.
    */
    native_handle_type release();

    /** Shut down part or all of the socket (best-effort).

        Unix stream sockets are full-duplex: each direction (send and
        receive) operates independently. This function allows you to
        close one or both directions without destroying the socket.

        @li @ref shutdown_send signals end-of-stream to the peer: their
            subsequent reads will complete with `capy::cond::eof` after
            they drain any data already in flight. You can still
            receive data from the peer until they also close their
            send direction. This is the cleanest way to end a session
            — preferable to @ref close() because it gives the peer an
            explicit EOF rather than tearing the socket down abruptly.

        @li @ref shutdown_receive disables reading on the socket. The
            peer is not informed and may continue sending; data
            already buffered or arriving later is discarded.

        @li @ref shutdown_both combines both effects.

        When the peer shuts down their send direction, subsequent read
        operations on this socket complete with `capy::cond::eof`. Use
        the portable condition test rather than comparing error codes
        directly:

        @code
        auto [ec, n] = co_await sock.read_some(buffer);
        if (ec == capy::cond::eof)
        {
            // Peer closed their send direction
        }
        @endcode

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

        Same semantics as @ref shutdown(shutdown_type) but reports
        syscall errors via @p ec instead of swallowing them.

        @param what Which direction to shut down.
        @param ec Set to the error code on failure.
    */
    void shutdown(shutdown_type what, std::error_code& ec) noexcept;

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

        The fd is adopted and registered with the platform reactor.
        Used by @ref make_local_stream_pair to wrap `socketpair`
        fds.

        @param fd The file descriptor to adopt. Must be a valid,
            open Unix domain stream socket.

        @par Preconditions
        The socket must not already be open.

        @throws std::logic_error if the precondition is violated
            (the socket is already open).
        @throws std::system_error on any other failure (e.g. the
            fd is not a Unix domain stream socket, or backend
            configuration fails).
    */
    void assign(int fd);

    corosio::local_endpoint local_endpoint() const noexcept;

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
