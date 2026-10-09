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
openssl_stream::make_implementation(
    capy::any_stream stream,
    detail::tls_owned_transport owned,
    tls_context const& ctx)
{
    // Session creation is deferred to handshake time (the engine's
    // prepare hook builds it lazily), so a session setup failure
    // surfaces through the handshake completion instead of leaving a
    // null implementation behind.
    return new implementation(std::move(stream), owned, ctx);
}

namespace {

template<class Impl>
void
release_driver(Impl* impl) noexcept
{
    if (!impl)
        return;
    impl->orphan();
    if (impl->release_ref())
        delete impl;
}

} // namespace

openssl_stream::~openssl_stream()
{
    release_driver(impl_);
}

openssl_stream::openssl_stream(openssl_stream&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr))
{
}

openssl_stream&
openssl_stream::operator=(openssl_stream&& other) noexcept
{
    if (this != &other)
    {
        release_driver(impl_);
        impl_ = std::exchange(other.impl_, nullptr);
    }
    return *this;
}

capy::any_stream&
openssl_stream::next_layer() noexcept
{
    return impl_->stream();
}

capy::any_stream const&
openssl_stream::next_layer() const noexcept
{
    return impl_->stream();
}

capy::io_task<std::size_t>
openssl_stream::do_read_some(
    capy::detail::mutable_buffer_array<capy::detail::max_iovec_> buffers)
{
    return detail::driver_read_some(
        detail::driver_ref<implementation>(impl_), buffers);
}

capy::io_task<std::size_t>
openssl_stream::do_write_some(
    capy::detail::const_buffer_array<capy::detail::max_iovec_> buffers)
{
    return detail::driver_write_some(
        detail::driver_ref<implementation>(impl_), buffers);
}

capy::io_task<>
openssl_stream::handshake(tls_role role)
{
    return detail::driver_handshake(
        detail::driver_ref<implementation>(impl_), role);
}

capy::io_task<>
openssl_stream::shutdown()
{
    return detail::driver_shutdown(detail::driver_ref<implementation>(impl_));
}

void
openssl_stream::cancel() noexcept
{
    if (impl_)
        impl_->cancel_transport();
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
