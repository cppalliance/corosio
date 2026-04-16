//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_TYPES_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_TYPES_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_SELECT

/* Named per-backend types for the select reactor.

   Each class is a final, named wrapper around the parameterized
   reactor_*_impl templates. Forward-declarable from backend.hpp
   so the concrete layer never pulls in platform headers.
*/

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/native/detail/select/select_traits.hpp>
#include <boost/corosio/native/detail/select/select_scheduler.hpp>
#include <boost/corosio/native/detail/reactor/reactor_backend.hpp>

namespace boost::corosio::detail {

// Forward declarations for cross-references.
class select_tcp_socket;
class select_tcp_service;
class select_tcp_acceptor;
class select_tcp_acceptor_service;
class select_udp_socket;
class select_udp_service;
class select_local_stream_socket;
class select_local_stream_service;
class select_local_stream_acceptor;
class select_local_stream_acceptor_service;
class select_local_datagram_socket;
class select_local_datagram_service;

// --- Stream sockets ---

class select_tcp_socket final
    : public reactor_stream_socket_impl<
          select_tcp_socket, select_traits, select_tcp_service,
          select_tcp_acceptor, tcp_socket::implementation, endpoint>
{
    using base_type = reactor_stream_socket_impl<
        select_tcp_socket, select_traits, select_tcp_service,
        select_tcp_acceptor, tcp_socket::implementation, endpoint>;
    friend select_tcp_service;
public:
    explicit select_tcp_socket(select_tcp_service& svc) noexcept
        : base_type(svc) {}
};

class select_local_stream_socket final
    : public reactor_stream_socket_impl<
          select_local_stream_socket, select_traits,
          select_local_stream_service, select_local_stream_acceptor,
          local_stream_socket::implementation, corosio::local_endpoint>
{
    using base_type = reactor_stream_socket_impl<
        select_local_stream_socket, select_traits,
        select_local_stream_service, select_local_stream_acceptor,
        local_stream_socket::implementation, corosio::local_endpoint>;
    friend select_local_stream_service;
public:
    explicit select_local_stream_socket(select_local_stream_service& svc) noexcept
        : base_type(svc) {}

    native_handle_type release_socket() noexcept override
    {
        hook_ = {};
        return this->do_release_socket();
    }
};

// --- Datagram sockets ---

class select_udp_socket final
    : public reactor_dgram_socket_impl<
          select_udp_socket, select_traits, select_udp_service,
          select_tcp_acceptor, udp_socket::implementation, endpoint>
{
    using base_type = reactor_dgram_socket_impl<
        select_udp_socket, select_traits, select_udp_service,
        select_tcp_acceptor, udp_socket::implementation, endpoint>;
    friend select_udp_service;
public:
    explicit select_udp_socket(select_udp_service& svc) noexcept
        : base_type(svc) {}
};

class select_local_datagram_socket final
    : public reactor_dgram_socket_impl<
          select_local_datagram_socket, select_traits,
          select_local_datagram_service, select_tcp_acceptor,
          local_datagram_socket::implementation, corosio::local_endpoint>
{
    using base_type = reactor_dgram_socket_impl<
        select_local_datagram_socket, select_traits,
        select_local_datagram_service, select_tcp_acceptor,
        local_datagram_socket::implementation, corosio::local_endpoint>;
    friend select_local_datagram_service;
public:
    explicit select_local_datagram_socket(select_local_datagram_service& svc) noexcept
        : base_type(svc) {}

    std::error_code shutdown(corosio::shutdown_type what) noexcept override
    {
        return this->do_shutdown(static_cast<int>(what));
    }

    std::error_code bind(corosio::local_endpoint ep) noexcept override
    {
        return this->do_bind(ep);
    }

    native_handle_type release_socket() noexcept override
    {
        return this->do_release_socket();
    }
};

// --- Acceptors ---

class select_tcp_acceptor final
    : public reactor_acceptor_impl<
          select_tcp_acceptor, select_traits,
          select_tcp_acceptor_service, select_tcp_socket,
          tcp_acceptor::implementation, endpoint>
{
    using base_type = reactor_acceptor_impl<
        select_tcp_acceptor, select_traits,
        select_tcp_acceptor_service, select_tcp_socket,
        tcp_acceptor::implementation, endpoint>;
    friend select_tcp_acceptor_service;
public:
    explicit select_tcp_acceptor(select_tcp_acceptor_service& svc) noexcept
        : base_type(svc) {}
};

class select_local_stream_acceptor final
    : public reactor_acceptor_impl<
          select_local_stream_acceptor, select_traits,
          select_local_stream_acceptor_service,
          select_local_stream_socket,
          local_stream_acceptor::implementation, corosio::local_endpoint>
{
    using base_type = reactor_acceptor_impl<
        select_local_stream_acceptor, select_traits,
        select_local_stream_acceptor_service,
        select_local_stream_socket,
        local_stream_acceptor::implementation, corosio::local_endpoint>;
    friend select_local_stream_acceptor_service;
public:
    explicit select_local_stream_acceptor(
        select_local_stream_acceptor_service& svc) noexcept
        : base_type(svc) {}

    native_handle_type release_socket() noexcept override
    {
        return this->do_release_socket();
    }
};

// --- Services ---

class BOOST_COROSIO_DECL select_tcp_service final
    : public reactor_tcp_service_impl<
          select_tcp_service, select_traits, select_tcp_socket>
{
    using base_type = reactor_tcp_service_impl<
        select_tcp_service, select_traits, select_tcp_socket>;
public:
    explicit select_tcp_service(capy::execution_context& ctx)
        : base_type(ctx) {}
};

class BOOST_COROSIO_DECL select_local_stream_service final
    : public reactor_local_stream_service_impl<
          select_local_stream_service, select_traits,
          select_local_stream_socket>
{
    using base_type = reactor_local_stream_service_impl<
        select_local_stream_service, select_traits,
        select_local_stream_socket>;
public:
    explicit select_local_stream_service(capy::execution_context& ctx)
        : base_type(ctx) {}
};

class BOOST_COROSIO_DECL select_udp_service final
    : public reactor_udp_service_impl<
          select_udp_service, select_traits, select_udp_socket>
{
    using base_type = reactor_udp_service_impl<
        select_udp_service, select_traits, select_udp_socket>;
public:
    explicit select_udp_service(capy::execution_context& ctx)
        : base_type(ctx) {}
};

class BOOST_COROSIO_DECL select_local_datagram_service final
    : public reactor_local_dgram_service_impl<
          select_local_datagram_service, select_traits,
          select_local_datagram_socket>
{
    using base_type = reactor_local_dgram_service_impl<
        select_local_datagram_service, select_traits,
        select_local_datagram_socket>;
public:
    explicit select_local_datagram_service(capy::execution_context& ctx)
        : base_type(ctx) {}
};

class BOOST_COROSIO_DECL select_tcp_acceptor_service final
    : public reactor_acceptor_service_impl<
          select_tcp_acceptor_service, select_traits,
          tcp_acceptor_service, select_tcp_acceptor,
          select_tcp_service, endpoint>
{
    using base_type = reactor_acceptor_service_impl<
        select_tcp_acceptor_service, select_traits,
        tcp_acceptor_service, select_tcp_acceptor,
        select_tcp_service, endpoint>;
public:
    explicit select_tcp_acceptor_service(capy::execution_context& ctx)
        : base_type(ctx) {}
};

class BOOST_COROSIO_DECL select_local_stream_acceptor_service final
    : public reactor_acceptor_service_impl<
          select_local_stream_acceptor_service, select_traits,
          local_stream_acceptor_service,
          select_local_stream_acceptor,
          select_local_stream_service, corosio::local_endpoint>
{
    using base_type = reactor_acceptor_service_impl<
        select_local_stream_acceptor_service, select_traits,
        local_stream_acceptor_service,
        select_local_stream_acceptor,
        select_local_stream_service, corosio::local_endpoint>;
public:
    explicit select_local_stream_acceptor_service(capy::execution_context& ctx)
        : base_type(ctx) {}
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_TYPES_HPP
