//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Michael Vandeberg
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/wolfssl_stream.hpp>
#include <boost/corosio/detail/config.hpp>

#include "detail/engine.hpp"
#include "src/tls/detail/engine_driver.hpp"

// The driver logic lives once in detail::engine_driver (see its
// header for the architecture); this TU only binds it to the WolfSSL
// engine and plumbs the public stream surface through.

namespace boost::corosio {

// Readable failure if the engine drifts from the driver's surface.
static_assert(detail::tls_engine<detail::wolfssl::engine>);

struct wolfssl_stream::implementation
    : detail::engine_driver<detail::wolfssl::engine>
{
    using engine_driver::engine_driver;
};

wolfssl_stream::implementation*
wolfssl_stream::make_implementation(capy::any_stream& stream, tls_context const& ctx)
{
    // Session creation is deferred to handshake time when the role is
    // known (the engine's prepare hook builds it from the role's
    // cached native context).
    return new implementation(stream, ctx);
}

wolfssl_stream::~wolfssl_stream()
{
    delete impl_;
}

wolfssl_stream::wolfssl_stream(wolfssl_stream&& other) noexcept
    : stream_(std::move(other.stream_))
    , impl_(other.impl_)
{
    other.impl_ = nullptr;
    if (impl_)
        impl_->rebind_stream(stream_);
}

wolfssl_stream&
wolfssl_stream::operator=(wolfssl_stream&& other) noexcept
{
    if (this != &other)
    {
        delete impl_;
        stream_     = std::move(other.stream_);
        impl_       = other.impl_;
        other.impl_ = nullptr;
        if (impl_)
            impl_->rebind_stream(stream_);
    }
    return *this;
}

capy::io_task<std::size_t>
wolfssl_stream::do_read_some(
    capy::detail::mutable_buffer_array<capy::detail::max_iovec_> buffers)
{
    co_return co_await impl_->do_read_some(buffers);
}

capy::io_task<std::size_t>
wolfssl_stream::do_write_some(
    capy::detail::const_buffer_array<capy::detail::max_iovec_> buffers)
{
    co_return co_await impl_->do_write_some(buffers);
}

capy::io_task<>
wolfssl_stream::handshake(tls_role role)
{
    co_return co_await impl_->do_handshake(role);
}

capy::io_task<>
wolfssl_stream::shutdown()
{
    co_return co_await impl_->do_shutdown();
}

void
wolfssl_stream::reset()
{
    impl_->reset();
}

void
wolfssl_stream::set_hostname(std::string_view hostname)
{
    impl_->set_hostname(hostname);
}

std::string_view
wolfssl_stream::name() const noexcept
{
    return "wolfssl";
}

std::string_view
wolfssl_stream::alpn_protocol() const noexcept
{
    return impl_->alpn_protocol();
}

} // namespace boost::corosio
