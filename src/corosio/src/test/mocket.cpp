//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/test/mocket.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/capy/buffers/slice.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/test/fuse.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace boost::corosio::test {

//------------------------------------------------------------------------------

mocket::
~mocket() = default;

mocket::
mocket(
    capy::execution_context& ctx,
    capy::test::fuse& f,
    std::size_t max_read_size,
    std::size_t max_write_size)
    : sock_(ctx)
    , fuse_(&f)
    , max_read_size_(max_read_size)
    , max_write_size_(max_write_size)
{
    if (max_read_size == 0)
        detail::throw_logic_error("mocket: max_read_size cannot be 0");
    if (max_write_size == 0)
        detail::throw_logic_error("mocket: max_write_size cannot be 0");
}

mocket::
mocket(mocket&& other) noexcept
    : sock_(std::move(other.sock_))
    , provide_(std::move(other.provide_))
    , expect_(std::move(other.expect_))
    , fuse_(other.fuse_)
    , max_read_size_(other.max_read_size_)
    , max_write_size_(other.max_write_size_)
{
    other.fuse_ = nullptr;
}

mocket&
mocket::
operator=(mocket&& other) noexcept
{
    if (this != &other)
    {
        sock_ = std::move(other.sock_);
        provide_ = std::move(other.provide_);
        expect_ = std::move(other.expect_);
        fuse_ = other.fuse_;
        max_read_size_ = other.max_read_size_;
        max_write_size_ = other.max_write_size_;
        other.fuse_ = nullptr;
    }
    return *this;
}

void
mocket::
provide(std::string s)
{
    provide_.append(std::move(s));
}

void
mocket::
expect(std::string s)
{
    expect_.append(std::move(s));
}

std::error_code
mocket::
close()
{
    if (!sock_.is_open())
        return {};

    // Verify test expectations
    if (!expect_.empty())
    {
        fuse_->fail();
        sock_.close();
        return capy::error::test_failure;
    }
    if (!provide_.empty())
    {
        fuse_->fail();
        sock_.close();
        return capy::error::test_failure;
    }

    sock_.close();
    return {};
}

void
mocket::
cancel()
{
    sock_.cancel();
}

bool
mocket::
is_open() const noexcept
{
    return sock_.is_open();
}

//------------------------------------------------------------------------------

std::pair<mocket, tcp_socket>
make_mocket_pair(
    capy::execution_context& ctx,
    capy::test::fuse& f,
    std::size_t max_read_size,
    std::size_t max_write_size)
{
    auto& ioc = static_cast<io_context&>(ctx);
    auto ex = ioc.get_executor();

    // Create the mocket
    mocket m(ctx, f, max_read_size, max_write_size);

    // Create the peer socket
    tcp_socket peer(ctx);

    std::error_code accept_ec;
    std::error_code connect_ec;
    bool accept_done = false;
    bool connect_done = false;

    // Use ephemeral port (0) - OS assigns an available port
    tcp_acceptor acc(ctx);
    auto listen_ec = acc.listen(endpoint(ipv4_address::loopback(), 0));
    if (listen_ec)
        throw std::runtime_error("mocket listen failed: " + listen_ec.message());
    auto port = acc.local_endpoint().port();

    // Open peer socket for connect
    peer.open();

    // Create a tcp_socket to receive the accepted connection
    tcp_socket accepted_socket(ctx);

    // Launch accept operation
    capy::run_async(ex)(
        [](tcp_acceptor& a, tcp_socket& s,
           std::error_code& ec_out, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await a.accept(s);
            ec_out = ec;
            done_out = true;
        }(acc, accepted_socket, accept_ec, accept_done));

    // Launch connect operation
    capy::run_async(ex)(
        [](tcp_socket& s, endpoint ep,
           std::error_code& ec_out, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await s.connect(ep);
            ec_out = ec;
            done_out = true;
        }(peer, endpoint(ipv4_address::loopback(), port),
          connect_ec, connect_done));

    // Run until both complete
    ioc.run();
    ioc.restart();

    // Check for errors
    if (!accept_done || accept_ec)
    {
        std::fprintf(stderr, "make_mocket_pair: accept failed (done=%d, ec=%s)\n",
            accept_done, accept_ec.message().c_str());
        acc.close();
        throw std::runtime_error("mocket accept failed");
    }

    if (!connect_done || connect_ec)
    {
        std::fprintf(stderr, "make_mocket_pair: connect failed (done=%d, ec=%s)\n",
            connect_done, connect_ec.message().c_str());
        acc.close();
        accepted_socket.close();
        throw std::runtime_error("mocket connect failed");
    }

    // Transfer the accepted socket to mocket
    m.socket() = std::move(accepted_socket);

    acc.close();

    return {std::move(m), std::move(peer)};
}

} // namespace boost::corosio::test
