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

#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/detail/config.hpp>

#include "detail/engine.hpp"
#include "src/tls/detail/engine_driver.hpp"

// The driver logic lives once in detail::engine_driver (see its
// header for the architecture); this TU only binds it to the OpenSSL
// engine and plumbs the public stream surface through.

namespace boost::corosio {

// Readable failure if the engine drifts from the driver's surface.
static_assert(detail::tls_engine<detail::openssl::engine>);

struct openssl_stream::implementation
    : detail::engine_driver<detail::openssl::engine>
{
    using engine_driver::engine_driver;
};

openssl_stream::implementation*
openssl_stream::make_implementation(capy::any_stream& stream, tls_context const& ctx)
{
    auto* p = new implementation(stream, ctx);

    auto ec = p->engine().init(p->context());
    if (ec)
    {
        delete p;
        return nullptr;
    }

    return p;
}

openssl_stream::~openssl_stream()
{
    delete impl_;
}

openssl_stream::openssl_stream(openssl_stream&& other) noexcept
    : stream_(std::move(other.stream_))
    , impl_(other.impl_)
{
    other.impl_ = nullptr;
    if (impl_)
        impl_->rebind_stream(stream_);
}

openssl_stream&
openssl_stream::operator=(openssl_stream&& other) noexcept
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
openssl_stream::do_read_some(
    capy::detail::mutable_buffer_array<capy::detail::max_iovec_> buffers)
{
    co_return co_await impl_->do_read_some(buffers);
}

capy::io_task<std::size_t>
openssl_stream::do_write_some(
    capy::detail::const_buffer_array<capy::detail::max_iovec_> buffers)
{
    co_return co_await impl_->do_write_some(buffers);
}

capy::io_task<>
openssl_stream::handshake(tls_role role)
{
    co_return co_await impl_->do_handshake(role);
}

capy::io_task<>
openssl_stream::shutdown()
{
    co_return co_await impl_->do_shutdown();
}

void
openssl_stream::reset()
{
    impl_->reset();
}

void
openssl_stream::set_hostname(std::string_view hostname)
{
    impl_->set_hostname(hostname);
}

std::string_view
openssl_stream::name() const noexcept
{
    return "openssl";
}

std::string_view
openssl_stream::alpn_protocol() const noexcept
{
    return impl_->alpn_protocol();
}

} // namespace boost::corosio
