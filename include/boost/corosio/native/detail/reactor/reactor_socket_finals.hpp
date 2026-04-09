//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SOCKET_FINALS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SOCKET_FINALS_HPP

/* Parameterized final socket and acceptor types for reactor backends.

   These templates are instantiated per-backend via Traits to produce
   the concrete socket types used by the public API. Each final type
   is a thin wrapper adding only protocol-specific details (ImplBase,
   Endpoint) to the CRTP base classes.
*/

#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/shutdown_type.hpp>

#include <boost/corosio/native/detail/reactor/reactor_stream_socket.hpp>
#include <boost/corosio/native/detail/reactor/reactor_datagram_socket.hpp>
#include <boost/corosio/native/detail/reactor/reactor_acceptor.hpp>
#include <boost/corosio/native/detail/reactor/reactor_stream_ops.hpp>
#include <boost/corosio/native/detail/reactor/reactor_datagram_ops.hpp>

#include <boost/corosio/detail/tcp_acceptor_service.hpp>
#include <boost/corosio/detail/local_stream_acceptor_service.hpp>
#include <boost/corosio/native/detail/make_err.hpp>

namespace boost::corosio::detail {

// ============================================================
// Forward declarations
// ============================================================

template<class Traits, class ImplBase, class Endpoint> class reactor_stream_socket_final;
template<class Traits, class ImplBase, class Endpoint> class reactor_dgram_socket_final;
template<class Traits, class AccImplBase, class Endpoint> class reactor_acceptor_final;

template<class Traits, class SocketFinal> class reactor_tcp_service_final;
template<class Traits, class SocketFinal> class reactor_local_stream_service_final;
template<class Traits, class SocketFinal> class reactor_udp_service_final;
template<class Traits, class SocketFinal> class reactor_local_dgram_service_final;
template<class Traits, class ServiceBase, class AccFinal, class StreamServiceFinal, class Endpoint> class reactor_acceptor_service_final;

// ============================================================
// Op type aliases
// ============================================================

template<class Traits, class Endpoint>
using stream_socket_t = reactor_stream_socket_final<Traits,
    std::conditional_t<std::is_same_v<Endpoint, endpoint>,
        tcp_socket::implementation,
        local_stream_socket::implementation>,
    Endpoint>;

template<class Traits, class Endpoint>
using stream_acceptor_t = reactor_acceptor_final<Traits,
    std::conditional_t<std::is_same_v<Endpoint, endpoint>,
        tcp_acceptor::implementation,
        local_stream_acceptor::implementation>,
    Endpoint>;

template<class Traits, class Endpoint>
using stream_base_op = reactor_stream_base_op<
    Traits, stream_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, Endpoint>, Endpoint>;

template<class Traits, class Endpoint>
using stream_connect_op = reactor_stream_connect_op<
    Traits, stream_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, Endpoint>, Endpoint>;

template<class Traits, class Endpoint>
using stream_read_op = reactor_stream_read_op<
    Traits, stream_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, Endpoint>, Endpoint>;

template<class Traits, class Endpoint>
using stream_write_op = reactor_stream_write_op<
    Traits, stream_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, Endpoint>, Endpoint>;

template<class Traits, class Endpoint>
using stream_accept_op = reactor_stream_accept_op<
    Traits, stream_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, Endpoint>, Endpoint>;

template<class Traits, class Endpoint>
using dgram_socket_t = reactor_dgram_socket_final<Traits,
    std::conditional_t<std::is_same_v<Endpoint, endpoint>,
        udp_socket::implementation,
        local_datagram_socket::implementation>,
    Endpoint>;

template<class Traits, class Endpoint>
using dgram_connect_op = reactor_dgram_connect_op<
    Traits, dgram_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, endpoint>, Endpoint>;

template<class Traits, class Endpoint>
using dgram_send_to_op = reactor_dgram_send_to_op<
    Traits, dgram_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, endpoint>, Endpoint>;

template<class Traits, class Endpoint>
using dgram_recv_from_op = reactor_dgram_recv_from_op<
    Traits, dgram_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, endpoint>, Endpoint>;

template<class Traits, class Endpoint>
using dgram_send_op = reactor_dgram_send_op<
    Traits, dgram_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, endpoint>, Endpoint>;

template<class Traits, class Endpoint>
using dgram_recv_op = reactor_dgram_recv_op<
    Traits, dgram_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, endpoint>, Endpoint>;

// ============================================================
// Stream socket final
// ============================================================

// release_socket below cannot be marked 'override' unconditionally: it
// overrides a pure virtual only when ImplBase is local_stream_socket::
// implementation. For the tcp_socket::implementation instantiation the
// base has no such virtual, so 'override' would fail to compile. Scope-
// suppress clang's -Winconsistent-missing-override for the overriding
// instantiation. clang-tidy's modernize-use-override is silenced
// separately via NOLINTNEXTLINE below.
BOOST_COROSIO_CLANG_WARNING_PUSH
BOOST_COROSIO_CLANG_WARNING_DISABLE("-Winconsistent-missing-override")

template<class Traits, class ImplBase, class Endpoint>
class reactor_stream_socket_final final
    : public reactor_stream_socket<
          reactor_stream_socket_final<Traits, ImplBase, Endpoint>,
          std::conditional_t<std::is_same_v<Endpoint, endpoint>,
              reactor_tcp_service_final<
                  Traits, reactor_stream_socket_final<Traits, ImplBase, Endpoint>>,
              reactor_local_stream_service_final<
                  Traits, reactor_stream_socket_final<Traits, ImplBase, Endpoint>>>,
          stream_connect_op<Traits, Endpoint>,
          stream_read_op<Traits, Endpoint>,
          stream_write_op<Traits, Endpoint>,
          typename Traits::desc_state_type,
          ImplBase,
          Endpoint>
{
    using service_type = std::conditional_t<std::is_same_v<Endpoint, endpoint>,
        reactor_tcp_service_final<Traits, reactor_stream_socket_final>,
        reactor_local_stream_service_final<Traits, reactor_stream_socket_final>>;
    friend service_type;

public:
    using impl_base_type = ImplBase;

    /// Per-socket hook state (e.g., kqueue SO_LINGER tracking).
    [[no_unique_address]] typename Traits::stream_socket_hook hook_;

    explicit reactor_stream_socket_final(service_type& svc) noexcept
        : reactor_stream_socket_final::reactor_stream_socket(svc)
    {
    }

    ~reactor_stream_socket_final() override = default;

    std::error_code set_option(
        int level, int optname,
        void const* data, std::size_t size) noexcept override
    {
        return hook_.on_set_option(this->fd_, level, optname, data, size);
    }

    // Shadows reactor_stream_socket::close_socket so the hook fires on
    // every fd close path (user close(), service shutdown/destroy,
    // do_assign_fd reuse). The hook needs the fd before do_close_socket
    // resets it to -1.
    void close_socket() noexcept
    {
        hook_.pre_shutdown(this->fd_);
        this->do_close_socket();
    }

    // Overrides local_stream_socket::implementation::release_socket().
    // Cannot use 'override' — tcp_socket::implementation has no such method.
    // NOLINTNEXTLINE(modernize-use-override)
    native_handle_type release_socket() noexcept
    {
        hook_ = {};
        return this->do_release_socket();
    }
};

BOOST_COROSIO_CLANG_WARNING_POP

// ============================================================
// Datagram socket final
// ============================================================

// shutdown/bind/release_socket below cannot be marked 'override'
// unconditionally: they override pure virtuals only when ImplBase is
// local_datagram_socket::implementation. For the udp_socket::implementation
// instantiation the base has no such virtuals, so 'override' would fail
// to compile. Scope-suppress clang's -Winconsistent-missing-override for
// the overriding instantiation. clang-tidy's modernize-use-override is
// silenced separately via NOLINTNEXTLINE below.
BOOST_COROSIO_CLANG_WARNING_PUSH
BOOST_COROSIO_CLANG_WARNING_DISABLE("-Winconsistent-missing-override")

template<class Traits, class ImplBase, class Endpoint>
class reactor_dgram_socket_final final
    : public reactor_datagram_socket<
          reactor_dgram_socket_final<Traits, ImplBase, Endpoint>,
          std::conditional_t<std::is_same_v<Endpoint, endpoint>,
              reactor_udp_service_final<
                  Traits, reactor_dgram_socket_final<Traits, ImplBase, Endpoint>>,
              reactor_local_dgram_service_final<
                  Traits, reactor_dgram_socket_final<Traits, ImplBase, Endpoint>>>,
          dgram_connect_op<Traits, Endpoint>,
          dgram_send_to_op<Traits, Endpoint>,
          dgram_recv_from_op<Traits, Endpoint>,
          dgram_send_op<Traits, Endpoint>,
          dgram_recv_op<Traits, Endpoint>,
          typename Traits::desc_state_type,
          ImplBase,
          Endpoint>
{
    using service_type = std::conditional_t<std::is_same_v<Endpoint, endpoint>,
        reactor_udp_service_final<Traits, reactor_dgram_socket_final>,
        reactor_local_dgram_service_final<Traits, reactor_dgram_socket_final>>;
    friend service_type;

public:
    using impl_base_type = ImplBase;

    explicit reactor_dgram_socket_final(service_type& svc) noexcept
        : reactor_dgram_socket_final::reactor_datagram_socket(svc)
    {
    }

    ~reactor_dgram_socket_final() override = default;

    // Overrides local_datagram_socket pure virtuals.
    // Cannot use 'override' — udp_socket::implementation has no such methods.
    // NOLINTNEXTLINE(modernize-use-override)
    std::error_code shutdown(corosio::shutdown_type what) noexcept
    {
        return this->do_shutdown(static_cast<int>(what));
    }

    // NOLINTNEXTLINE(modernize-use-override)
    std::error_code bind(Endpoint ep) noexcept
    {
        return this->do_bind(ep);
    }

    // NOLINTNEXTLINE(modernize-use-override)
    native_handle_type release_socket() noexcept
    {
        return this->do_release_socket();
    }
};

BOOST_COROSIO_CLANG_WARNING_POP

// ============================================================
// Acceptor final
// ============================================================

template<class Traits, class Endpoint>
using stream_service_for = std::conditional_t<std::is_same_v<Endpoint, endpoint>,
    reactor_tcp_service_final<Traits, stream_socket_t<Traits, Endpoint>>,
    reactor_local_stream_service_final<Traits, stream_socket_t<Traits, Endpoint>>>;

// release_socket below cannot be marked 'override' unconditionally: it
// overrides a pure virtual only when AccImplBase is
// local_stream_acceptor::implementation. For the tcp_acceptor::implementation
// instantiation the base has no such virtual, so 'override' would fail to
// compile. Scope-suppress clang's -Winconsistent-missing-override for the
// overriding instantiation. clang-tidy's modernize-use-override is silenced
// separately via NOLINTNEXTLINE below.
BOOST_COROSIO_CLANG_WARNING_PUSH
BOOST_COROSIO_CLANG_WARNING_DISABLE("-Winconsistent-missing-override")

template<class Traits, class AccImplBase, class Endpoint>
class reactor_acceptor_final final
    : public reactor_acceptor<
          reactor_acceptor_final<Traits, AccImplBase, Endpoint>,
          reactor_acceptor_service_final<
              Traits,
              std::conditional_t<std::is_same_v<Endpoint, endpoint>,
                  tcp_acceptor_service, local_stream_acceptor_service>,
              reactor_acceptor_final<Traits, AccImplBase, Endpoint>,
              stream_service_for<Traits, Endpoint>,
              Endpoint>,
          stream_base_op<Traits, Endpoint>,
          stream_accept_op<Traits, Endpoint>,
          typename Traits::desc_state_type,
          AccImplBase,
          Endpoint>
{
    using acc_service_type = reactor_acceptor_service_final<
        Traits,
        std::conditional_t<std::is_same_v<Endpoint, endpoint>,
            tcp_acceptor_service, local_stream_acceptor_service>,
        reactor_acceptor_final,
        stream_service_for<Traits, Endpoint>,
        Endpoint>;
    friend acc_service_type;

public:
    explicit reactor_acceptor_final(acc_service_type& svc) noexcept
        : reactor_acceptor_final::reactor_acceptor(svc)
    {
    }

    ~reactor_acceptor_final() override = default;

    using impl_base_type = AccImplBase;

    // NOLINTNEXTLINE(modernize-use-override)
    native_handle_type release_socket() noexcept
    {
        return this->do_release_socket();
    }

    std::coroutine_handle<> accept(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::stop_token,
        std::error_code*,
        io_object::implementation**) override;
};

BOOST_COROSIO_CLANG_WARNING_POP

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SOCKET_FINALS_HPP
