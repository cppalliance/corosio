//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_MOCKET_HPP
#define BOOST_COROSIO_TEST_MOCKET_HPP

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/buffers/buffer_copy.hpp>
#include <boost/capy/buffers/make_buffer.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/test/fuse.hpp>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace boost::corosio::test {

/** A mock socket for testing I/O operations.

    This class provides a testable socket-like interface where data
    can be staged for reading and expected data can be validated on
    writes. A mocket is paired with a regular socket using
    @ref make_mocket_pair, allowing bidirectional communication testing.

    When reading, data comes from the `provide()` buffer first.
    When writing, data is validated against the `expect()` buffer.
    Once buffers are exhausted, I/O passes through to the underlying
    socket connection.

    Satisfies the `capy::Stream` concept.

    @tparam Socket The underlying socket type (default `tcp_socket`).

    @par Thread Safety
    Not thread-safe. All operations must occur on a single thread.
    All coroutines using the mocket must be suspended when calling
    `expect()` or `provide()`.

    @see make_mocket_pair
*/
template<class Socket = tcp_socket>
class basic_mocket
{
    Socket sock_;
    std::string provide_;
    std::string expect_;
    capy::test::fuse fuse_;
    std::size_t max_read_size_;
    std::size_t max_write_size_;

    template<class MutableBufferSequence>
    std::size_t consume_provide(MutableBufferSequence const& buffers) noexcept;

    template<class ConstBufferSequence>
    bool validate_expect(
        ConstBufferSequence const& buffers, std::size_t& bytes_written);

public:
    template<class MutableBufferSequence>
    class read_some_awaitable;

    template<class ConstBufferSequence>
    class write_some_awaitable;

    /** Destructor.
    */
    ~basic_mocket() = default;

    /** Construct a mocket.

        @param ctx The execution context for the socket.
        @param f The fuse for error injection testing.
        @param max_read_size Maximum bytes per read operation.
        @param max_write_size Maximum bytes per write operation.
    */
    basic_mocket(
        capy::execution_context& ctx,
        capy::test::fuse f         = {},
        std::size_t max_read_size  = std::size_t(-1),
        std::size_t max_write_size = std::size_t(-1))
        : sock_(ctx)
        , fuse_(std::move(f))
        , max_read_size_(max_read_size)
        , max_write_size_(max_write_size)
    {
        if (max_read_size == 0)
            detail::throw_logic_error("mocket: max_read_size cannot be 0");
        if (max_write_size == 0)
            detail::throw_logic_error("mocket: max_write_size cannot be 0");
    }

    /** Move constructor.
    */
    basic_mocket(basic_mocket&& other) noexcept
        : sock_(std::move(other.sock_))
        , provide_(std::move(other.provide_))
        , expect_(std::move(other.expect_))
        , fuse_(std::move(other.fuse_))
        , max_read_size_(other.max_read_size_)
        , max_write_size_(other.max_write_size_)
    {
    }

    /** Move assignment.
    */
    basic_mocket& operator=(basic_mocket&& other) noexcept
    {
        if (this != &other)
        {
            sock_           = std::move(other.sock_);
            provide_        = std::move(other.provide_);
            expect_         = std::move(other.expect_);
            fuse_           = other.fuse_;
            max_read_size_  = other.max_read_size_;
            max_write_size_ = other.max_write_size_;
        }
        return *this;
    }

    basic_mocket(basic_mocket const&)            = delete;
    basic_mocket& operator=(basic_mocket const&) = delete;

    /** Return the execution context.

        @return Reference to the execution context that owns this mocket.
    */
    capy::execution_context& context() const noexcept
    {
        return sock_.context();
    }

    /** Return the underlying socket.

        @return Reference to the underlying socket.
    */
    Socket& socket() noexcept
    {
        return sock_;
    }

    /** Stage data for reads.

        Appends the given string to this mocket's provide buffer.
        When `read_some` is called, it will receive this data first
        before reading from the underlying socket.

        @param s The data to provide.

        @pre All coroutines using this mocket must be suspended.
    */
    void provide(std::string const& s)
    {
        provide_.append(s);
    }

    /** Set expected data for writes.

        Appends the given string to this mocket's expect buffer.
        When the caller writes to this mocket, the written data
        must match the expected data. On mismatch, `fuse::fail()`
        is called.

        @param s The expected data.

        @pre All coroutines using this mocket must be suspended.
    */
    void expect(std::string const& s)
    {
        expect_.append(s);
    }

    /** Close the mocket and verify test expectations.

        Closes the underlying socket and verifies that both the
        `expect()` and `provide()` buffers are empty. If either
        buffer contains unconsumed data, returns `test_failure`
        and calls `fuse::fail()`.

        @return An error code indicating success or failure.
            Returns `error::test_failure` if buffers are not empty.
    */
    std::error_code close()
    {
        if (!sock_.is_open())
            return {};

        if (!expect_.empty())
        {
            fuse_.fail();
            sock_.close();
            return capy::error::test_failure;
        }
        if (!provide_.empty())
        {
            fuse_.fail();
            sock_.close();
            return capy::error::test_failure;
        }

        sock_.close();
        return {};
    }

    /** Cancel pending I/O operations.

        Cancels any pending asynchronous operations on the underlying
        socket. Outstanding operations complete with `cond::canceled`.
    */
    void cancel()
    {
        sock_.cancel();
    }

    /** Check if the mocket is open.

        @return `true` if the mocket is open.
    */
    bool is_open() const noexcept
    {
        return sock_.is_open();
    }

    /** Initiate an asynchronous read operation.

        Reads available data into the provided buffer sequence. If the
        provide buffer has data, it is consumed first. Otherwise, the
        operation delegates to the underlying socket.

        @param buffers The buffer sequence to read data into.

        @return An awaitable yielding `(error_code, std::size_t)`.
    */
    template<class MutableBufferSequence>
    auto read_some(MutableBufferSequence const& buffers)
    {
        return read_some_awaitable<MutableBufferSequence>(*this, buffers);
    }

    /** Initiate an asynchronous write operation.

        Writes data from the provided buffer sequence. If the expect
        buffer has data, it is validated. Otherwise, the operation
        delegates to the underlying socket.

        @param buffers The buffer sequence containing data to write.

        @return An awaitable yielding `(error_code, std::size_t)`.
    */
    template<class ConstBufferSequence>
    auto write_some(ConstBufferSequence const& buffers)
    {
        return write_some_awaitable<ConstBufferSequence>(*this, buffers);
    }
};

/// Default mocket type using `tcp_socket`.
using mocket = basic_mocket<>;

template<class Socket>
template<class MutableBufferSequence>
std::size_t
basic_mocket<Socket>::consume_provide(
    MutableBufferSequence const& buffers) noexcept
{
    auto n =
        capy::buffer_copy(buffers, capy::make_buffer(provide_), max_read_size_);
    provide_.erase(0, n);
    return n;
}

template<class Socket>
template<class ConstBufferSequence>
bool
basic_mocket<Socket>::validate_expect(
    ConstBufferSequence const& buffers, std::size_t& bytes_written)
{
    if (expect_.empty())
        return true;

    // Build the write data up to max_write_size_
    std::string written;
    auto total = capy::buffer_size(buffers);
    if (total > max_write_size_)
        total = max_write_size_;
    written.resize(total);
    capy::buffer_copy(capy::make_buffer(written), buffers, max_write_size_);

    // Check if written data matches expect prefix
    auto const match_size = (std::min)(written.size(), expect_.size());
    if (std::memcmp(written.data(), expect_.data(), match_size) != 0)
    {
        fuse_.fail();
        bytes_written = 0;
        return false;
    }

    // Consume matched portion
    expect_.erase(0, match_size);
    bytes_written = written.size();
    return true;
}

template<class Socket>
template<class MutableBufferSequence>
class basic_mocket<Socket>::read_some_awaitable
{
    using sock_awaitable = decltype(std::declval<Socket&>().read_some(
        std::declval<MutableBufferSequence>()));

    basic_mocket* m_;
    MutableBufferSequence buffers_;
    std::size_t n_ = 0;
    union
    {
        char dummy_;
        sock_awaitable underlying_;
    };
    bool sync_ = true;

public:
    read_some_awaitable(basic_mocket& m, MutableBufferSequence buffers) noexcept
        : m_(&m)
        , buffers_(std::move(buffers))
    {
    }

    ~read_some_awaitable()
    {
        if (!sync_)
            underlying_.~sock_awaitable();
    }

    read_some_awaitable(read_some_awaitable&& other) noexcept
        : m_(other.m_)
        , buffers_(std::move(other.buffers_))
        , n_(other.n_)
        , sync_(other.sync_)
    {
        if (!sync_)
        {
            new (&underlying_) sock_awaitable(std::move(other.underlying_));
            other.underlying_.~sock_awaitable();
            other.sync_ = true;
        }
    }

    read_some_awaitable(read_some_awaitable const&)            = delete;
    read_some_awaitable& operator=(read_some_awaitable const&) = delete;
    read_some_awaitable& operator=(read_some_awaitable&&)      = delete;

    bool await_ready()
    {
        if (!m_->provide_.empty())
        {
            n_ = m_->consume_provide(buffers_);
            return true;
        }
        new (&underlying_) sock_awaitable(m_->sock_.read_some(buffers_));
        sync_ = false;
        return underlying_.await_ready();
    }

    template<class... Args>
    auto await_suspend(Args&&... args)
    {
        return underlying_.await_suspend(std::forward<Args>(args)...);
    }

    capy::io_result<std::size_t> await_resume()
    {
        if (sync_)
            return {{}, n_};
        return underlying_.await_resume();
    }
};

template<class Socket>
template<class ConstBufferSequence>
class basic_mocket<Socket>::write_some_awaitable
{
    using sock_awaitable = decltype(std::declval<Socket&>().write_some(
        std::declval<ConstBufferSequence>()));

    basic_mocket* m_;
    ConstBufferSequence buffers_;
    std::size_t n_ = 0;
    std::error_code ec_;
    union
    {
        char dummy_;
        sock_awaitable underlying_;
    };
    bool sync_ = true;

public:
    write_some_awaitable(basic_mocket& m, ConstBufferSequence buffers) noexcept
        : m_(&m)
        , buffers_(std::move(buffers))
    {
    }

    ~write_some_awaitable()
    {
        if (!sync_)
            underlying_.~sock_awaitable();
    }

    write_some_awaitable(write_some_awaitable&& other) noexcept
        : m_(other.m_)
        , buffers_(std::move(other.buffers_))
        , n_(other.n_)
        , ec_(other.ec_)
        , sync_(other.sync_)
    {
        if (!sync_)
        {
            new (&underlying_) sock_awaitable(std::move(other.underlying_));
            other.underlying_.~sock_awaitable();
            other.sync_ = true;
        }
    }

    write_some_awaitable(write_some_awaitable const&)            = delete;
    write_some_awaitable& operator=(write_some_awaitable const&) = delete;
    write_some_awaitable& operator=(write_some_awaitable&&)      = delete;

    bool await_ready()
    {
        if (!m_->expect_.empty())
        {
            if (!m_->validate_expect(buffers_, n_))
            {
                ec_ = capy::error::test_failure;
                n_  = 0;
            }
            return true;
        }
        new (&underlying_) sock_awaitable(m_->sock_.write_some(buffers_));
        sync_ = false;
        return underlying_.await_ready();
    }

    template<class... Args>
    auto await_suspend(Args&&... args)
    {
        return underlying_.await_suspend(std::forward<Args>(args)...);
    }

    capy::io_result<std::size_t> await_resume()
    {
        if (sync_)
            return {ec_, n_};
        return underlying_.await_resume();
    }
};

/** Create a mocket paired with a socket.

    Creates a mocket and a socket connected via loopback.
    Data written to one can be read from the other.

    The mocket has fuse checks enabled via `maybe_fail()` and
    supports provide/expect buffers for test instrumentation.
    The socket is the "peer" end with no test instrumentation.

    Optional max_read_size and max_write_size parameters limit the
    number of bytes transferred per I/O operation on the mocket,
    simulating chunked network delivery for testing purposes.

    @tparam Socket The socket type (default `tcp_socket`).
    @tparam Acceptor The acceptor type (default `tcp_acceptor`).

    @param ctx The I/O context for the sockets.
    @param f The fuse for error injection testing.
    @param max_read_size Maximum bytes per read operation (default unlimited).
    @param max_write_size Maximum bytes per write operation (default unlimited).

    @return A pair of (mocket, socket).

    @note Mockets are not thread-safe and must be used in a
        single-threaded, deterministic context.
*/
template<class Socket = tcp_socket, class Acceptor = tcp_acceptor>
std::pair<basic_mocket<Socket>, Socket>
make_mocket_pair(
    io_context& ctx,
    capy::test::fuse f         = {},
    std::size_t max_read_size  = std::size_t(-1),
    std::size_t max_write_size = std::size_t(-1))
{
    auto ex = ctx.get_executor();

    basic_mocket<Socket> m(ctx, std::move(f), max_read_size, max_write_size);

    Socket peer(ctx);

    std::error_code accept_ec;
    std::error_code connect_ec;
    bool accept_done  = false;
    bool connect_done = false;

    Acceptor acc(ctx);
    acc.open();
    acc.set_option(socket_option::reuse_address(true));
    if (auto bind_ec = acc.bind(endpoint(ipv4_address::loopback(), 0)))
        throw std::runtime_error(
            "mocket bind failed: " + bind_ec.message());
    if (auto listen_ec = acc.listen())
        throw std::runtime_error(
            "mocket listen failed: " + listen_ec.message());
    auto port = acc.local_endpoint().port();

    peer.open();

    Socket accepted_socket(ctx);

    capy::run_async(ex)(
        [](Acceptor& a, Socket& s, std::error_code& ec_out,
           bool& done_out) -> capy::task<> {
            auto [ec] = co_await a.accept(s);
            ec_out    = ec;
            done_out  = true;
        }(acc, accepted_socket, accept_ec, accept_done));

    capy::run_async(ex)(
        [](Socket& s, endpoint ep, std::error_code& ec_out,
           bool& done_out) -> capy::task<> {
            auto [ec] = co_await s.connect(ep);
            ec_out    = ec;
            done_out  = true;
        }(peer, endpoint(ipv4_address::loopback(), port), connect_ec,
                           connect_done));

    ctx.run();
    ctx.restart();

    if (!accept_done || accept_ec)
    {
        std::fprintf(
            stderr, "make_mocket_pair: accept failed (done=%d, ec=%s)\n",
            accept_done, accept_ec.message().c_str());
        acc.close();
        throw std::runtime_error("mocket accept failed");
    }

    if (!connect_done || connect_ec)
    {
        std::fprintf(
            stderr, "make_mocket_pair: connect failed (done=%d, ec=%s)\n",
            connect_done, connect_ec.message().c_str());
        acc.close();
        accepted_socket.close();
        throw std::runtime_error("mocket connect failed");
    }

    m.socket() = std::move(accepted_socket);

    acc.close();

    return {std::move(m), std::move(peer)};
}

} // namespace boost::corosio::test

#endif
