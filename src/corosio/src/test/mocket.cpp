//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/test/mocket.hpp>
#include <boost/corosio/acceptor.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/socket.hpp>
#include "src/detail/intrusive.hpp"
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/test/fuse.hpp>
#include <boost/url/ipv4_address.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>

#ifdef _WIN32
#include <process.h>  // _getpid()
#else
#include <unistd.h>   // getpid()
#endif

namespace boost::corosio::test {

namespace {

constexpr std::size_t max_buffers = 8;
using buffer_array = std::array<capy::mutable_buffer, max_buffers>;

} // namespace

//------------------------------------------------------------------------------

class mocket_service;

class mocket_impl
    : public io_stream::io_stream_impl
    , public detail::intrusive_list<mocket_impl>::node
{
    mocket_service& svc_;
    capy::test::fuse& fuse_;
    socket sock_;
    std::string provide_;
    std::string expect_;
    mocket_impl* peer_ = nullptr;
    bool check_fuse_;

public:
    mocket_impl(
        mocket_service& svc,
        capy::execution_context& ctx,
        capy::test::fuse& f,
        bool check_fuse);

    void set_peer(mocket_impl* peer) noexcept
    {
        peer_ = peer;
    }

    socket& get_socket() noexcept
    {
        return sock_;
    }

    void provide(std::string s)
    {
        provide_.append(std::move(s));
    }

    void expect(std::string s)
    {
        expect_.append(std::move(s));
    }

    system::error_code close();

    bool is_open() const noexcept
    {
        return sock_.is_open();
    }

    void release() override;

    void read_some(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        io_buffer_param buffers,
        std::stop_token token,
        system::error_code* ec,
        std::size_t* bytes_transferred) override;

    void write_some(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        io_buffer_param buffers,
        std::stop_token token,
        system::error_code* ec,
        std::size_t* bytes_transferred) override;

private:
    std::size_t
    fill_from_provide(
        buffer_array const& bufs,
        std::size_t count);

    bool
    validate_expect(
        buffer_array const& bufs,
        std::size_t count,
        std::size_t total_size);
};

//------------------------------------------------------------------------------

class mocket_service
    : public capy::execution_context::service
{
    capy::execution_context& ctx_;
    detail::intrusive_list<mocket_impl> impls_;

public:
    explicit mocket_service(capy::execution_context& ctx)
        : ctx_(ctx)
    {
    }

    mocket_impl&
    create_impl(capy::test::fuse& f, bool check_fuse)
    {
        auto* impl = new mocket_impl(*this, ctx_, f, check_fuse);
        impls_.push_back(impl);
        return *impl;
    }

    void
    destroy_impl(mocket_impl& impl)
    {
        impls_.remove(&impl);
        delete &impl;
    }

protected:
    void shutdown() override
    {
        while (auto* impl = impls_.pop_front())
            delete impl;
    }
};

//------------------------------------------------------------------------------

mocket_impl::
mocket_impl(
    mocket_service& svc,
    capy::execution_context& ctx,
    capy::test::fuse& f,
    bool check_fuse)
    : svc_(svc)
    , fuse_(f)
    , sock_(ctx)
    , check_fuse_(check_fuse)
{
}

system::error_code
mocket_impl::
close()
{
    // Verify test expectations
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

void
mocket_impl::
release()
{
    svc_.destroy_impl(*this);
}

std::size_t
mocket_impl::
fill_from_provide(
    buffer_array const& bufs,
    std::size_t count)
{
    if (!peer_ || peer_->provide_.empty())
        return 0;

    std::size_t total = 0;
    auto& src = peer_->provide_;

    for (std::size_t i = 0; i < count && !src.empty(); ++i)
    {
        auto const n = std::min(bufs[i].size(), src.size());
        std::memcpy(bufs[i].data(), src.data(), n);
        src.erase(0, n);
        total += n;
    }
    return total;
}

bool
mocket_impl::
validate_expect(
    buffer_array const& bufs,
    std::size_t count,
    std::size_t total_size)
{
    if (expect_.empty())
        return true;

    // Build the write data
    std::string written;
    written.reserve(total_size);
    for (std::size_t i = 0; i < count; ++i)
    {
        written.append(
            static_cast<char const*>(bufs[i].data()),
            bufs[i].size());
    }

    // Check if written data matches expect prefix
    auto const n = std::min(written.size(), expect_.size());
    if (std::memcmp(written.data(), expect_.data(), n) != 0)
    {
        fuse_.fail();
        return false;
    }

    // Consume matched portion
    expect_.erase(0, n);
    return true;
}

void
mocket_impl::
read_some(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    io_buffer_param buffers,
    std::stop_token token,
    system::error_code* ec,
    std::size_t* bytes_transferred)
{
    (void)token;
    // Fuse check for m1 only
    if (check_fuse_)
    {
        auto fail_ec = fuse_.maybe_fail();
        if (fail_ec)
        {
            *ec = fail_ec;
            *bytes_transferred = 0;
            d.dispatch(capy::coro{h}).resume();
            return;
        }
    }

    // Check if peer has staged data - if so, serve from provide buffer
    if (peer_ && !peer_->provide_.empty())
    {
        // Extract buffers only when we need them for staged data
        buffer_array bufs{};
        std::size_t count = buffers.copy_to(bufs.data(), max_buffers);

        std::size_t n = fill_from_provide(bufs, count);
        *ec = {};
        *bytes_transferred = n;
        d.dispatch(capy::coro{h}).resume();
        return;
    }

    // Pass through to the real socket (don't extract buffers - forward as-is)
    sock_.get_impl()->read_some(h, d, buffers, token, ec, bytes_transferred);
}

void
mocket_impl::
write_some(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    io_buffer_param buffers,
    std::stop_token token,
    system::error_code* ec,
    std::size_t* bytes_transferred)
{
    (void)token;
    // Fuse check for m1 only
    if (check_fuse_)
    {
        auto fail_ec = fuse_.maybe_fail();
        if (fail_ec)
        {
            *ec = fail_ec;
            *bytes_transferred = 0;
            d.dispatch(capy::coro{h}).resume();
            return;
        }
    }

    // Check if we have staged expectations to validate
    if (!expect_.empty())
    {
        // Extract buffers only when we need them for validation
        buffer_array bufs{};
        std::size_t count = buffers.copy_to(bufs.data(), max_buffers);

        // Calculate total size
        std::size_t total_size = 0;
        for (std::size_t i = 0; i < count; ++i)
            total_size += bufs[i].size();

        if (!validate_expect(bufs, count, total_size))
        {
            *ec = capy::error::test_failure;
            *bytes_transferred = 0;
            d.dispatch(capy::coro{h}).resume();
            return;
        }

        // If all expected data was validated, report success
        *ec = {};
        *bytes_transferred = total_size;
        d.dispatch(capy::coro{h}).resume();
        return;
    }

    // Pass through to the real socket (don't extract buffers - forward as-is)
    sock_.get_impl()->write_some(h, d, buffers, token, ec, bytes_transferred);
}

//------------------------------------------------------------------------------

mocket_impl*
mocket::
get_impl() const noexcept
{
    return static_cast<mocket_impl*>(impl_);
}

mocket::
~mocket()
{
    if (impl_)
        impl_->release();
    impl_ = nullptr;
}

mocket::
mocket(mocket_impl* impl) noexcept
    : io_stream(impl->get_socket().context())
{
    impl_ = impl;
}

mocket::
mocket(mocket&& other) noexcept
    : io_stream(other.context())
{
    impl_ = other.impl_;
    other.impl_ = nullptr;
}

mocket&
mocket::
operator=(mocket&& other) noexcept
{
    if (this != &other)
    {
        if (impl_)
            impl_->release();
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

void
mocket::
provide(std::string s)
{
    get_impl()->provide(std::move(s));
}

void
mocket::
expect(std::string s)
{
    get_impl()->expect(std::move(s));
}

system::error_code
mocket::
close()
{
    if (!impl_)
        return {};
    return get_impl()->close();
}

bool
mocket::
is_open() const noexcept
{
    return impl_ && get_impl()->is_open();
}

//------------------------------------------------------------------------------

namespace {

// Use atomic for thread safety when tests run in parallel
std::atomic<std::uint16_t> next_test_port{0};

std::uint16_t
get_test_port() noexcept
{
    // Use a wide port range in the dynamic/ephemeral range (49152-65535)
    constexpr std::uint16_t port_base = 49152;
    constexpr std::uint16_t port_range = 16383;

    // Include PID to avoid port collisions between parallel test processes.
    // On Windows with SO_REUSEADDR, multiple processes can bind the same port,
    // causing connections to go to the wrong listener ("port stealing").
    // By using different port ranges per process, we avoid this issue.
#ifdef _WIN32
    auto pid = static_cast<std::uint32_t>(_getpid());
#else
    auto pid = static_cast<std::uint32_t>(getpid());
#endif
    // Mix the PID bits to spread processes across the port range
    auto pid_offset = static_cast<std::uint16_t>((pid * 7919) % port_range);

    auto offset = next_test_port.fetch_add(1, std::memory_order_relaxed);
    return static_cast<std::uint16_t>(port_base + ((pid_offset + offset) % port_range));
}

} // namespace

std::pair<mocket, mocket>
make_mockets(capy::execution_context& ctx, capy::test::fuse& f)
{
    auto& svc = ctx.use_service<mocket_service>();

    // Create the two implementations
    auto& impl1 = svc.create_impl(f, true);   // m1 checks fuse
    auto& impl2 = svc.create_impl(f, false);  // m2 does not

    // Link them as peers
    impl1.set_peer(&impl2);
    impl2.set_peer(&impl1);

    auto& ioc = static_cast<io_context&>(ctx);
    auto ex = ioc.get_executor();

    system::error_code accept_ec;
    system::error_code connect_ec;
    bool accept_done = false;
    bool connect_done = false;

    // Try multiple ports in case of conflicts (TIME_WAIT, parallel tests, etc.)
    std::uint16_t port = 0;
    acceptor acc(ctx);
    bool listening = false;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        port = get_test_port();
        try
        {
            acc.listen(endpoint(urls::ipv4_address::loopback(), port));
            listening = true;
            break;
        }
        catch (const system::system_error&)
        {
            acc.close();
            acc = acceptor(ctx);
        }
    }
    if (!listening)
    {
        std::fprintf(stderr, "make_mockets: failed to find available port after 20 attempts\n");
        throw std::runtime_error("make_mockets: failed to find available port");
    }

    // Open impl2's socket for connect
    impl2.get_socket().open();

    // Create a socket to receive the accepted connection
    socket accepted_socket(ctx);

    // Launch accept operation
    // Note: Pass captures as parameters to store them in the coroutine frame,
    // avoiding use-after-scope when the lambda temporary is destroyed.
    capy::run_async(ex)(
        [](acceptor& a, socket& s,
           system::error_code& ec_out, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await a.accept(s);
            ec_out = ec;
            done_out = true;
        }(acc, accepted_socket, accept_ec, accept_done));

    // Launch connect operation
    capy::run_async(ex)(
        [](socket& s, endpoint ep,
           system::error_code& ec_out, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await s.connect(ep);
            ec_out = ec;
            done_out = true;
        }(impl2.get_socket(), endpoint(urls::ipv4_address::loopback(), port),
          connect_ec, connect_done));

    // Run until both complete
    ioc.run();
    ioc.restart();

    // Check for errors
    if (!accept_done || accept_ec)
    {
        std::fprintf(stderr, "make_mockets: accept failed (done=%d, ec=%s)\n",
            accept_done, accept_ec.message().c_str());
        acc.close();
        throw std::runtime_error("mocket accept failed");
    }

    if (!connect_done || connect_ec)
    {
        std::fprintf(stderr, "make_mockets: connect failed (done=%d, ec=%s)\n",
            connect_done, connect_ec.message().c_str());
        acc.close();
        accepted_socket.close();
        throw std::runtime_error("mocket connect failed");
    }

    // Transfer the accepted socket to impl1
    impl1.get_socket() = std::move(accepted_socket);

    acc.close();

    // Create the mocket wrappers
    mocket m1(&impl1);
    mocket m2(&impl2);

    return {std::move(m1), std::move(m2)};
}

} // namespace boost::corosio::test
