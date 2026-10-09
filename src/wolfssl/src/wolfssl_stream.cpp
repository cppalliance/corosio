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
wolfssl_stream::make_implementation(
    capy::any_stream stream,
    detail::tls_owned_transport owned,
    tls_context const& ctx)
{
    // Session creation is deferred to handshake time when the role is
    // known (the engine's prepare hook builds it from the role's
    // cached native context).
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

wolfssl_stream::~wolfssl_stream()
{
    release_driver(impl_);
}

wolfssl_stream::wolfssl_stream(wolfssl_stream&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr))
{
}

wolfssl_stream&
wolfssl_stream::operator=(wolfssl_stream&& other) noexcept
{
    if (this != &other)
    {
        release_driver(impl_);
        impl_ = std::exchange(other.impl_, nullptr);
    }
    return *this;
}

capy::any_stream&
wolfssl_stream::next_layer() noexcept
{
    return impl_->stream();
}

capy::any_stream const&
wolfssl_stream::next_layer() const noexcept
{
    return impl_->stream();
}

capy::io_task<std::size_t>
wolfssl_stream::do_read_some(
    capy::detail::mutable_buffer_array<capy::detail::max_iovec_> buffers)
{
    return detail::driver_read_some(
        detail::driver_ref<implementation>(impl_), buffers);
}

capy::io_task<std::size_t>
wolfssl_stream::do_write_some(
    capy::detail::const_buffer_array<capy::detail::max_iovec_> buffers)
{
    return detail::driver_write_some(
        detail::driver_ref<implementation>(impl_), buffers);
}

capy::io_task<>
wolfssl_stream::handshake(tls_role role)
{
    return detail::driver_handshake(
        detail::driver_ref<implementation>(impl_), role);
}

capy::io_task<>
wolfssl_stream::shutdown()
{
    return detail::driver_shutdown(detail::driver_ref<implementation>(impl_));
}

void
wolfssl_stream::cancel() noexcept
{
    if (impl_)
        impl_->cancel_transport();
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
