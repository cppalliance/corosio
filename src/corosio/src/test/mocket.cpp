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
#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <system_error>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/socket.hpp>
#include "src/detail/intrusive.hpp"
#include <boost/capy/buffers/slice.hpp>
#include <boost/capy/buffers/span.hpp>
#include <boost/capy/buffers/buffer_copy.hpp>
#include <boost/capy/buffers/make_buffer.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/test/fuse.hpp>

#include "src/detail/resume_coro.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>

namespace boost::corosio::test {

namespace {
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
    std::size_t max_read_size_;
    std::size_t max_write_size_;
    bool check_fuse_;

public:
    mocket_impl(
        mocket_service& svc,
        capy::execution_context& ctx,
        capy::test::fuse& f,
        bool check_fuse,
        std::size_t max_read_size,
        std::size_t max_write_size);

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

    std::error_code close();

    void cancel() noexcept
    {
        sock_.cancel();
    }

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
        std::error_code* ec,
        std::size_t* bytes_transferred) override;

    void write_some(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        io_buffer_param buffers,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes_transferred) override;

private:
    bool
    validate_expect(
        std::span<capy::mutable_buffer const> bufs,
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
    create_impl(
        capy::test::fuse& f,
        bool check_fuse,
        std::size_t max_read_size,
        std::size_t max_write_size)
    {
        auto* impl = new mocket_impl(
            *this, ctx_, f, check_fuse, max_read_size, max_write_size);
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
    bool check_fuse,
    std::size_t max_read_size,
    std::size_t max_write_size)
    : svc_(svc)
    , fuse_(f)
    , sock_(ctx)
    , max_read_size_(max_read_size)
    , max_write_size_(max_write_size)
    , check_fuse_(check_fuse)
{
    if (max_read_size == 0)
        detail::throw_logic_error("mocket: max_read_size cannot be 0");
    if (max_write_size == 0)
        detail::throw_logic_error("mocket: max_write_size cannot be 0");
}

std::error_code
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

bool
mocket_impl::
validate_expect(
    std::span<capy::mutable_buffer const> bufs,
    std::size_t total_size)
{
    if (expect_.empty())
        return true;

    // Build the write data
    std::string written;
    written.reserve(total_size);
    for (auto const& buf : bufs)
    {
        written.append(
            static_cast<char const*>(buf.data()),
            buf.size());
    }

    // Check if written data matches expect prefix
    auto const n = (std::min)(written.size(), expect_.size());
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
    std::error_code* ec,
    std::size_t* bytes_transferred)
{
    if (check_fuse_)
    {
        auto fail_ec = fuse_.maybe_fail();
        if (fail_ec)
        {
            *ec = fail_ec;
            *bytes_transferred = 0;
            detail::resume_coro(d, h);
            return;
        }
    }

    capy::mutable_buffer vb[detail::max_iovec_];
    std::span<capy::mutable_buffer> bufs(vb, buffers.copy_to(vb, detail::max_iovec_));
    capy::keep_span_prefix(bufs, max_read_size_);

    if (peer_ && !peer_->provide_.empty())
    {
        auto& src = peer_->provide_;
        auto n = capy::buffer_copy(bufs, capy::make_buffer(src));
        src.erase(0, n);

        *ec = {};
        *bytes_transferred = n;
        detail::resume_coro(d, h);
        return;
    }

    sock_.get_impl()->read_some(h, d, io_buffer_param(bufs), token, ec, bytes_transferred);
}

void
mocket_impl::
write_some(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    io_buffer_param buffers,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_transferred)
{
    if (check_fuse_)
    {
        auto fail_ec = fuse_.maybe_fail();
        if (fail_ec)
        {
            *ec = fail_ec;
            *bytes_transferred = 0;
            detail::resume_coro(d, h);
            return;
        }
    }

    capy::mutable_buffer vb[detail::max_iovec_];
    std::span<capy::mutable_buffer> bufs(vb, buffers.copy_to(vb, detail::max_iovec_));
    capy::keep_span_prefix(bufs, max_write_size_);
    auto total_size = capy::buffer_size(bufs);

    if (!expect_.empty())
    {
        if (!validate_expect(bufs, total_size))
        {
            *ec = capy::error::test_failure;
            *bytes_transferred = 0;
            detail::resume_coro(d, h);
            return;
        }

        *ec = {};
        *bytes_transferred = total_size;
        detail::resume_coro(d, h);
        return;
    }

    sock_.get_impl()->write_some(h, d, io_buffer_param(bufs), token, ec, bytes_transferred);
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

std::error_code
mocket::
close()
{
    if (!impl_)
        return {};
    return get_impl()->close();
}

void
mocket::
cancel()
{
    if (impl_)
        get_impl()->cancel();
}

bool
mocket::
is_open() const noexcept
{
    return impl_ && get_impl()->is_open();
}

//------------------------------------------------------------------------------

std::pair<mocket, mocket>
make_mockets(
    capy::execution_context& ctx,
    capy::test::fuse& f,
    std::size_t max_read_size,
    std::size_t max_write_size)
{
    auto& svc = ctx.use_service<mocket_service>();

    // Create the two implementations
    // m1 checks fuse and has size limits; m2 does not
    auto& impl1 = svc.create_impl(f, true, max_read_size, max_write_size);
    auto& impl2 = svc.create_impl(f, false, std::size_t(-1), std::size_t(-1));

    // Link them as peers
    impl1.set_peer(&impl2);
    impl2.set_peer(&impl1);

    auto& ioc = static_cast<io_context&>(ctx);
    auto ex = ioc.get_executor();

    std::error_code accept_ec;
    std::error_code connect_ec;
    bool accept_done = false;
    bool connect_done = false;

    // Use ephemeral port (0) - OS assigns an available port
    acceptor acc(ctx);
    acc.listen(endpoint(ipv4_address::loopback(), 0));
    auto port = acc.local_endpoint().port();

    // Open impl2's socket for connect
    impl2.get_socket().open();

    // Create a socket to receive the accepted connection
    socket accepted_socket(ctx);

    // Launch accept operation
    // Note: Pass captures as parameters to store them in the coroutine frame,
    // avoiding use-after-scope when the lambda temporary is destroyed.
    capy::run_async(ex)(
        [](acceptor& a, socket& s,
           std::error_code& ec_out, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await a.accept(s);
            ec_out = ec;
            done_out = true;
        }(acc, accepted_socket, accept_ec, accept_done));

    // Launch connect operation
    capy::run_async(ex)(
        [](socket& s, endpoint ep,
           std::error_code& ec_out, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await s.connect(ep);
            ec_out = ec;
            done_out = true;
        }(impl2.get_socket(), endpoint(ipv4_address::loopback(), port),
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
