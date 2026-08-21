//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_tcp_acceptor_service.hpp>
#else
#include <boost/corosio/detail/tcp_acceptor_service.hpp>
#endif

#include <boost/corosio/detail/except.hpp>

namespace boost::corosio {

#if BOOST_COROSIO_HAS_IOCP
namespace {

// On Windows SO_REUSEADDR grants bind-over ("hijack") rights instead
// of TIME_WAIT reuse, so a second listener would share the port
// silently. SO_EXCLUSIVEADDRUSE restores the POSIX contract: the
// collision surfaces as WSAEADDRINUSE (errc::address_in_use).
struct exclusive_address_use
{
    int value_ = 1;

    static int level() noexcept { return SOL_SOCKET; }
    static int name() noexcept { return SO_EXCLUSIVEADDRUSE; }
    void const* data() const noexcept { return &value_; }
    std::size_t size() const noexcept { return sizeof(value_); }
};

} // namespace
#endif

tcp_acceptor::~tcp_acceptor()
{
    close();
}

tcp_acceptor::tcp_acceptor(capy::execution_context& ctx)
#if BOOST_COROSIO_HAS_IOCP
    : io_object(create_handle<detail::win_tcp_acceptor_service>(ctx))
#else
    : io_object(create_handle<detail::tcp_acceptor_service>(ctx))
#endif
{
}

tcp_acceptor::tcp_acceptor(
    capy::execution_context& ctx, endpoint ep, int backlog)
    : tcp_acceptor(ctx)
{
    if (auto ec = open(ep.is_v6() ? tcp::v6() : tcp::v4()))
        detail::throw_system_error(ec, "tcp_acceptor");
#if BOOST_COROSIO_HAS_IOCP
    set_option(exclusive_address_use{});
#else
    set_option(socket_option::reuse_address(true));
#endif
    if (auto ec = bind(ep))
        detail::throw_system_error(ec, "tcp_acceptor");
    if (auto ec = listen(backlog))
        detail::throw_system_error(ec, "tcp_acceptor");
}

std::error_code
tcp_acceptor::open(tcp proto) noexcept
{
    if (is_open())
        return {};

#if BOOST_COROSIO_HAS_IOCP
    auto& svc = static_cast<detail::win_tcp_acceptor_service&>(h_.service());
#else
    auto& svc = static_cast<detail::tcp_acceptor_service&>(h_.service());
#endif
    std::error_code ec = svc.open_acceptor_socket(
        *static_cast<tcp_acceptor::implementation*>(h_.get()), proto.family(),
        proto.type(), proto.protocol());
    return ec;
}

std::error_code
tcp_acceptor::assign(native_handle_type fd) noexcept
{
#if BOOST_COROSIO_HAS_IOCP
    auto& svc = static_cast<detail::win_tcp_acceptor_service&>(h_.service());
#else
    auto& svc = static_cast<detail::tcp_acceptor_service&>(h_.service());
#endif
    std::error_code ec = svc.assign_socket(
        *static_cast<tcp_acceptor::implementation*>(h_.get()), fd);
    return ec;
}

native_handle_type
tcp_acceptor::release()
{
    if (!is_open())
        detail::throw_system_error(
            make_error_code(std::errc::bad_file_descriptor),
            "tcp_acceptor::release");
    return get().release_socket();
}

native_handle_type
tcp_acceptor::native_handle() const noexcept
{
    if (!is_open())
    {
#if BOOST_COROSIO_HAS_IOCP
        return static_cast<native_handle_type>(~0ull); // INVALID_SOCKET
#else
        return -1;
#endif
    }
    return get().native_handle();
}

std::error_code
tcp_acceptor::bind(endpoint ep) noexcept
{
    if (!is_open())
        return make_error_code(std::errc::bad_file_descriptor);
#if BOOST_COROSIO_HAS_IOCP
    auto& svc = static_cast<detail::win_tcp_acceptor_service&>(h_.service());
#else
    auto& svc = static_cast<detail::tcp_acceptor_service&>(h_.service());
#endif
    return svc.bind_acceptor(
        *static_cast<tcp_acceptor::implementation*>(h_.get()), ep);
}

std::error_code
tcp_acceptor::listen(int backlog) noexcept
{
    if (!is_open())
        return make_error_code(std::errc::bad_file_descriptor);
#if BOOST_COROSIO_HAS_IOCP
    auto& svc = static_cast<detail::win_tcp_acceptor_service&>(h_.service());
#else
    auto& svc = static_cast<detail::tcp_acceptor_service&>(h_.service());
#endif
    return svc.listen_acceptor(
        *static_cast<tcp_acceptor::implementation*>(h_.get()), backlog);
}

void
tcp_acceptor::close() noexcept
{
    if (!is_open())
        return;
    h_.service().close(h_);
}

void
tcp_acceptor::cancel()
{
    if (!is_open())
        return;
    get().cancel();
}

endpoint
tcp_acceptor::local_endpoint() const noexcept
{
    if (!is_open())
        return endpoint{};
    return get().local_endpoint();
}

} // namespace boost::corosio
