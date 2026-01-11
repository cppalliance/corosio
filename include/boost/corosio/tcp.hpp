//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TCP_HPP
#define BOOST_COROSIO_TCP_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/affine.hpp>
#include <boost/capy/execution_context.hpp>
#include <boost/url/ipv4_address.hpp>

#include <coroutine>
#include <cstdint>
#include <cstring>
#include <stop_token>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace boost {
namespace corosio {
namespace tcp {

/** An IPv4 TCP endpoint (address + port).

    This class represents an endpoint for TCP communication,
    consisting of an IPv4 address and a port number.

    @par Example
    @code
    tcp::endpoint ep(urls::ipv4_address::loopback(), 8080);
    @endcode
*/
struct endpoint
{
    urls::ipv4_address address;
    std::uint16_t port = 0;

    /** Default constructor.

        Creates an endpoint with the any address (0.0.0.0) and port 0.
    */
    endpoint() = default;

    /** Construct from address and port.

        @param addr The IPv4 address.
        @param p The port number in host byte order.
    */
    endpoint(urls::ipv4_address addr, std::uint16_t p) noexcept
        : address(addr)
        , port(p)
    {
    }

    /** Construct from port only.

        Uses the any address (0.0.0.0).

        @param p The port number in host byte order.
    */
    explicit endpoint(std::uint16_t p) noexcept
        : address(urls::ipv4_address::any())
        , port(p)
    {
    }

    /** Convert to sockaddr_in for system calls.

        @return A sockaddr_in structure in network byte order.
    */
    sockaddr_in to_sockaddr() const noexcept
    {
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        auto bytes = address.to_bytes();
        std::memcpy(&sa.sin_addr, bytes.data(), 4);
        return sa;
    }

    /** Create endpoint from sockaddr_in.

        @param sa The sockaddr_in structure.
        @return An endpoint.
    */
    static endpoint from_sockaddr(sockaddr_in const& sa) noexcept
    {
        urls::ipv4_address::bytes_type bytes;
        std::memcpy(bytes.data(), &sa.sin_addr, 4);
        return endpoint(urls::ipv4_address(bytes), ntohs(sa.sin_port));
    }

    /** Compare endpoints for equality. */
    friend bool operator==(endpoint const& a, endpoint const& b) noexcept
    {
        return a.address == b.address && a.port == b.port;
    }

    /** Compare endpoints for inequality. */
    friend bool operator!=(endpoint const& a, endpoint const& b) noexcept
    {
        return !(a == b);
    }
};

} // namespace tcp

namespace detail { class socket_impl; }
class socket;

namespace tcp {

/** A TCP acceptor for accepting incoming connections.

    This class provides server-side socket functionality for
    accepting incoming TCP connections using IOCP's AcceptEx.

    @par Example
    @code
    io_context ioc;
    tcp::acceptor acc(ioc);
    acc.open();
    acc.bind(tcp::endpoint(urls::ipv4_address::any(), 8080));
    acc.listen();
    
    socket client = co_await acc.accept();
    @endcode
*/
class acceptor
{
public:
    /** Awaitable for accept operations. */
    struct accept_awaitable
    {
        acceptor& a_;
        socket* peer_;
        std::stop_token token_;
        mutable std::error_code ec_;

        accept_awaitable(acceptor& a, socket& peer) noexcept
            : a_(a)
            , peer_(&peer)
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        std::error_code await_resume() const noexcept
        {
            if (token_.stop_requested())
                return std::make_error_code(std::errc::operation_canceled);
            return ec_;
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d) -> std::coroutine_handle<>;

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d,
            std::stop_token token) -> std::coroutine_handle<>
        {
            token_ = std::move(token);
            return await_suspend(h, d);
        }
    };

    BOOST_COROSIO_DECL
    ~acceptor();

    BOOST_COROSIO_DECL
    explicit acceptor(capy::execution_context& ctx);

    acceptor(acceptor&& other) noexcept;
    acceptor& operator=(acceptor&& other);

    acceptor(acceptor const&) = delete;
    acceptor& operator=(acceptor const&) = delete;

    /** Open the acceptor socket. */
    BOOST_COROSIO_DECL
    void open();

    /** Bind the acceptor to an endpoint.

        @param ep The local endpoint to bind to.
    */
    BOOST_COROSIO_DECL
    void bind(endpoint ep);

    /** Start listening for connections.

        @param backlog The maximum length of the pending connection queue.
    */
    BOOST_COROSIO_DECL
    void listen(int backlog = SOMAXCONN);

    /** Close the acceptor. */
    BOOST_COROSIO_DECL
    void close();

    /** Check if the acceptor is open. */
    bool is_open() const noexcept;

    /** Initiate an asynchronous accept operation.

        @param peer The socket to accept the connection into.
        @return An awaitable that completes with std::error_code.
    */
    accept_awaitable accept(socket& peer)
    {
        return accept_awaitable(*this, peer);
    }

    /** Cancel any pending operations. */
    BOOST_COROSIO_DECL
    void cancel();

private:
    friend struct accept_awaitable;

    BOOST_COROSIO_DECL
    void do_accept(
        std::coroutine_handle<>,
        capy::any_dispatcher,
        socket&,
        std::stop_token,
        std::error_code*);

    // Static callback for accept completion - transfers accepted socket to peer
    BOOST_COROSIO_DECL
    static void accept_transfer(
        void* peer_ptr,
        void* svc_ptr,
        detail::socket_impl* impl,
        SOCKET sock);

    capy::execution_context* ctx_;
    detail::socket_impl* impl_ = nullptr;
};

} // namespace tcp
} // namespace corosio
} // namespace boost

#endif
