//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SERVICE_FINALS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SERVICE_FINALS_HPP

/* Parameterized final service types for reactor backends.

   One service template per protocol (TCP, local stream, UDP, local
   datagram, TCP acceptor, local stream acceptor) because each abstract
   service base declares different virtual methods.
*/

#include <boost/corosio/native/detail/reactor/reactor_socket_finals.hpp>
#include <boost/corosio/native/detail/reactor/reactor_socket_service.hpp>
#include <boost/corosio/native/detail/reactor/reactor_acceptor_service.hpp>
#include <boost/corosio/detail/tcp_service.hpp>
#include <boost/corosio/detail/udp_service.hpp>
#include <boost/corosio/detail/local_stream_service.hpp>
#include <boost/corosio/detail/local_datagram_service.hpp>

#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/native/detail/make_err.hpp>

#include <system_error>
#include <type_traits>

#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

// ============================================================
// Shared socket creation helpers
// ============================================================

template<class Traits, class SocketFinal>
std::error_code
do_open_socket(
    SocketFinal* socket_impl,
    int family, int type, int protocol,
    bool is_ip) noexcept
{
    socket_impl->close_socket();

    int fd = Traits::create_socket(family, type, protocol);
    if (fd < 0)
        return make_err(errno);

    std::error_code ec = is_ip
        ? Traits::configure_ip_socket(fd, family)
        : Traits::configure_local_socket(fd);

    if (ec)
    {
        ::close(fd);
        return ec;
    }

    socket_impl->init_and_register(fd);
    return {};
}

template<class Traits, class SocketFinal>
std::error_code
do_assign_fd(
    SocketFinal* socket_impl,
    int fd,
    int expected_type) noexcept
{
    socket_impl->close_socket();

    // Validate that fd is actually an AF_UNIX socket of the expected
    // type BEFORE applying any backend setup. Otherwise we could
    // adopt (or worse, mutate flags on) a foreign fd — e.g., an
    // AF_INET SOCK_STREAM fd passed into local_stream_socket::assign,
    // or a SOCK_DGRAM fd passed into a stream socket. The caller
    // retains ownership of fd on any error from this function, so
    // doing the checks first leaves a rejected fd unmodified.
    {
        sockaddr_storage st{};
        socklen_t st_len = sizeof(st);
        if (::getsockname(
                fd, reinterpret_cast<sockaddr*>(&st), &st_len) != 0)
            return make_err(errno);
        if (st.ss_family != AF_UNIX)
            return make_err(EAFNOSUPPORT);

        int sock_type = 0;
        socklen_t opt_len = sizeof(sock_type);
        if (::getsockopt(
                fd, SOL_SOCKET, SO_TYPE, &sock_type, &opt_len) != 0)
            return make_err(errno);
        if (sock_type != expected_type)
            return make_err(EPROTOTYPE);
    }

    // Apply backend fd setup (O_NONBLOCK, FD_CLOEXEC, SO_NOSIGPIPE on kqueue,
    // FD_SETSIZE validation on select). On failure, the caller retains
    // ownership of fd — assign_socket reports the error and assign() throws
    // without closing.
    std::error_code ec = Traits::configure_local_socket(fd);
    if (ec)
        return ec;

    socket_impl->init_and_register(fd);

    // Best-effort: refresh endpoint caches so local_endpoint() and
    // remote_endpoint() reflect the actual fd state. Failures (e.g.
    // ENOTCONN from getpeername on an unconnected socket) are ignored;
    // for unnamed socketpair() fds the queries succeed but yield empty
    // endpoints, which is the correct cached state.
    using endpoint_type = std::remove_cvref_t<
        decltype(socket_impl->local_endpoint())>;

    endpoint_type local_ep{};
    sockaddr_storage local_storage{};
    socklen_t local_len = sizeof(local_storage);
    if (::getsockname(
            fd, reinterpret_cast<sockaddr*>(&local_storage), &local_len) == 0)
        local_ep = from_sockaddr_as(local_storage, local_len, endpoint_type{});

    endpoint_type remote_ep{};
    sockaddr_storage peer_storage{};
    socklen_t peer_len = sizeof(peer_storage);
    if (::getpeername(
            fd, reinterpret_cast<sockaddr*>(&peer_storage), &peer_len) == 0)
        remote_ep = from_sockaddr_as(peer_storage, peer_len, endpoint_type{});

    socket_impl->set_endpoints(local_ep, remote_ep);

    return {};
}

template<class Traits, class AccFinal>
std::error_code
do_open_acceptor(
    AccFinal* acc_impl,
    int family, int type, int protocol,
    bool is_ip) noexcept
{
    acc_impl->close_socket();

    int fd = Traits::create_socket(family, type, protocol);
    if (fd < 0)
        return make_err(errno);

    std::error_code ec = is_ip
        ? Traits::configure_ip_acceptor(fd, family)
        : Traits::configure_local_socket(fd);

    if (ec)
    {
        ::close(fd);
        return ec;
    }

    acc_impl->init_acceptor_fd(fd);
    return {};
}

// ============================================================
// TCP service
// ============================================================

template<class Traits, class SocketFinal>
class reactor_tcp_service_final final
    : public reactor_socket_service<
          reactor_tcp_service_final<Traits, SocketFinal>,
          tcp_service,
          typename Traits::scheduler_type,
          SocketFinal>
{
    using base_service = reactor_socket_service<
        reactor_tcp_service_final, tcp_service,
        typename Traits::scheduler_type, SocketFinal>;
    friend base_service;

public:
    explicit reactor_tcp_service_final(capy::execution_context& ctx)
        : base_service(ctx) {}

    std::error_code open_socket(
        tcp_socket::implementation& impl,
        int family, int type, int protocol) override
    {
        return do_open_socket<Traits>(
            static_cast<SocketFinal*>(&impl),
            family, type, protocol, true);
    }

    std::error_code bind_socket(
        tcp_socket::implementation& impl, endpoint ep) override
    {
        return static_cast<SocketFinal*>(&impl)->do_bind(ep);
    }

    void pre_shutdown(SocketFinal* impl) noexcept
    {
        impl->hook_.pre_shutdown(impl->native_handle());
    }

    void pre_destroy(SocketFinal* impl) noexcept
    {
        impl->hook_.pre_destroy(impl->native_handle());
    }
};

// ============================================================
// Local stream service
// ============================================================

template<class Traits, class SocketFinal>
class reactor_local_stream_service_final final
    : public reactor_socket_service<
          reactor_local_stream_service_final<Traits, SocketFinal>,
          local_stream_service,
          typename Traits::scheduler_type,
          SocketFinal>
{
    using base_service = reactor_socket_service<
        reactor_local_stream_service_final, local_stream_service,
        typename Traits::scheduler_type, SocketFinal>;
    friend base_service;

public:
    explicit reactor_local_stream_service_final(capy::execution_context& ctx)
        : base_service(ctx) {}

    std::error_code open_socket(
        local_stream_socket::implementation& impl,
        int family, int type, int protocol) override
    {
        return do_open_socket<Traits>(
            static_cast<SocketFinal*>(&impl),
            family, type, protocol, false);
    }

    std::error_code assign_socket(
        local_stream_socket::implementation& impl, int fd) override
    {
        return do_assign_fd<Traits>(
            static_cast<SocketFinal*>(&impl), fd, SOCK_STREAM);
    }
};

// ============================================================
// UDP service
// ============================================================

template<class Traits, class SocketFinal>
class reactor_udp_service_final final
    : public reactor_socket_service<
          reactor_udp_service_final<Traits, SocketFinal>,
          udp_service,
          typename Traits::scheduler_type,
          SocketFinal>
{
    using base_service = reactor_socket_service<
        reactor_udp_service_final, udp_service,
        typename Traits::scheduler_type, SocketFinal>;
    friend base_service;

public:
    explicit reactor_udp_service_final(capy::execution_context& ctx)
        : base_service(ctx) {}

    std::error_code open_datagram_socket(
        udp_socket::implementation& impl,
        int family, int type, int protocol) override
    {
        return do_open_socket<Traits>(
            static_cast<SocketFinal*>(&impl),
            family, type, protocol, true);
    }

    std::error_code bind_datagram(
        udp_socket::implementation& impl, endpoint ep) override
    {
        return static_cast<SocketFinal*>(&impl)->do_bind(ep);
    }
};

// ============================================================
// Local datagram service
// ============================================================

template<class Traits, class SocketFinal>
class reactor_local_dgram_service_final final
    : public reactor_socket_service<
          reactor_local_dgram_service_final<Traits, SocketFinal>,
          local_datagram_service,
          typename Traits::scheduler_type,
          SocketFinal>
{
    using base_service = reactor_socket_service<
        reactor_local_dgram_service_final, local_datagram_service,
        typename Traits::scheduler_type, SocketFinal>;
    friend base_service;

public:
    explicit reactor_local_dgram_service_final(capy::execution_context& ctx)
        : base_service(ctx) {}

    std::error_code open_socket(
        local_datagram_socket::implementation& impl,
        int family, int type, int protocol) override
    {
        return do_open_socket<Traits>(
            static_cast<SocketFinal*>(&impl),
            family, type, protocol, false);
    }

    std::error_code assign_socket(
        local_datagram_socket::implementation& impl, int fd) override
    {
        return do_assign_fd<Traits>(
            static_cast<SocketFinal*>(&impl), fd, SOCK_DGRAM);
    }

    std::error_code bind_socket(
        local_datagram_socket::implementation& impl,
        corosio::local_endpoint ep) override
    {
        return static_cast<SocketFinal*>(&impl)->do_bind(ep);
    }
};

// ============================================================
// Acceptor service
// ============================================================

template<class Traits, class ServiceBase, class AccFinal,
         class StreamServiceFinal, class Endpoint>
class reactor_acceptor_service_final final
    : public reactor_acceptor_service<
          reactor_acceptor_service_final<Traits, ServiceBase, AccFinal,
                                         StreamServiceFinal, Endpoint>,
          ServiceBase,
          typename Traits::scheduler_type,
          AccFinal,
          StreamServiceFinal>
{
    using base_service = reactor_acceptor_service<
        reactor_acceptor_service_final,
        ServiceBase,
        typename Traits::scheduler_type,
        AccFinal,
        StreamServiceFinal>;
    friend base_service;

public:
    explicit reactor_acceptor_service_final(capy::execution_context& ctx)
        : base_service(ctx)
    {
        // Look up the concrete stream service directly by its type.
        // Avoids dynamic_cast which can fail across template boundaries
        // on some platforms (FreeBSD clang RTTI/visibility).
        this->stream_svc_ =
            this->ctx_.template find_service<StreamServiceFinal>();
    }

    std::error_code open_acceptor_socket(
        typename AccFinal::impl_base_type& impl,
        int family, int type, int protocol) override
    {
        return do_open_acceptor<Traits>(
            static_cast<AccFinal*>(&impl),
            family, type, protocol,
            std::is_same_v<Endpoint, endpoint>);
    }

    std::error_code bind_acceptor(
        typename AccFinal::impl_base_type& impl,
        Endpoint ep) override
    {
        return static_cast<AccFinal*>(&impl)->do_bind(ep);
    }

    std::error_code listen_acceptor(
        typename AccFinal::impl_base_type& impl,
        int backlog) override
    {
        return static_cast<AccFinal*>(&impl)->do_listen(backlog);
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SERVICE_FINALS_HPP
