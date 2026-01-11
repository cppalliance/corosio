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

#include <boost/system/error_code.hpp>

#include <coroutine>
#include <cstdint>
#include <cstring>
#include <stop_token>

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

    This struct represents an endpoint for TCP communication,
    consisting of an IPv4 address and a port number. Endpoints
    are used to specify connection targets and bind addresses.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe.

    @par Example
    @code
    // Client connection target
    tcp::endpoint server(urls::ipv4_address::loopback(), 8080);

    // Server bind address (all interfaces)
    tcp::endpoint bind_addr(8080);
    @endcode
*/
struct endpoint
{
    /** The IPv4 address component. */
    urls::ipv4_address address;

    /** The port number in host byte order. */
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

        Uses the any address (0.0.0.0), which binds to all
        available network interfaces.

        @param p The port number in host byte order.
    */
    explicit endpoint(std::uint16_t p) noexcept
        : address(urls::ipv4_address::any())
        , port(p)
    {
    }

    /** Convert to sockaddr_in for system calls.

        @return A sockaddr_in structure with fields in network byte order.
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

        @param sa The sockaddr_in structure with fields in network byte order.

        @return An endpoint with address and port extracted from sa.
    */
    static endpoint from_sockaddr(sockaddr_in const& sa) noexcept
    {
        urls::ipv4_address::bytes_type bytes;
        std::memcpy(bytes.data(), &sa.sin_addr, 4);
        return endpoint(urls::ipv4_address(bytes), ntohs(sa.sin_port));
    }

    /** Compare endpoints for equality.

        @return `true` if both address and port are equal.
    */
    friend bool operator==(endpoint const& a, endpoint const& b) noexcept
    {
        return a.address == b.address && a.port == b.port;
    }

    /** Compare endpoints for inequality.

        @return `true` if address or port differs.
    */
    friend bool operator!=(endpoint const& a, endpoint const& b) noexcept
    {
        return !(a == b);
    }
};

} // namespace tcp

namespace detail { class win_socket_impl; }
class socket;

namespace tcp {

/** A TCP acceptor for accepting incoming connections.

    This class provides server-side socket functionality for
    accepting incoming TCP connections. On Windows, it uses
    IOCP with AcceptEx for efficient asynchronous acceptance.

    The acceptor must be opened, bound to an endpoint, and
    set to listen before accepting connections.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe.

    @par Example
    @code
    io_context ioc;
    tcp::acceptor acceptor(ioc);
    acceptor.open();
    acceptor.bind(tcp::endpoint(8080));
    acceptor.listen();

    corosio::socket client(ioc);
    client.open();
    auto ec = co_await acceptor.accept(client);
    @endcode
*/
class acceptor
{
public:
    struct accept_awaitable;

    /** Destructor.

        Closes the acceptor if open, cancelling any pending operations.
    */
    BOOST_COROSIO_DECL
    ~acceptor();

    /** Construct an acceptor from an execution context.

        @param ctx The execution context that will own this acceptor.
    */
    BOOST_COROSIO_DECL
    explicit acceptor(capy::execution_context& ctx);

    /** Move constructor.

        Transfers ownership of the acceptor resources.

        @param other The acceptor to move from.
    */
    acceptor(acceptor&& other) noexcept;

    /** Move assignment operator.

        Closes any existing acceptor and transfers ownership.

        @param other The acceptor to move from.

        @return Reference to this acceptor.
    */
    acceptor& operator=(acceptor&& other);

    acceptor(acceptor const&) = delete;
    acceptor& operator=(acceptor const&) = delete;

    /** Open the acceptor socket.

        Creates an IPv4 TCP socket for accepting connections
        and associates it with the platform reactor.

        @throws std::system_error on failure.
    */
    BOOST_COROSIO_DECL
    void open();

    /** Bind the acceptor to a local endpoint.

        Associates the acceptor with a local address and port.
        The acceptor must be open before calling this function.

        @param ep The local endpoint to bind to.

        @throws std::system_error on failure, including:
            - address_in_use: The port is already bound
            - address_not_available: The address is not local
    */
    BOOST_COROSIO_DECL
    void bind(endpoint ep);

    /** Start listening for incoming connections.

        Marks the socket as a passive socket that will accept
        incoming connections. The acceptor must be bound before
        calling this function.

        @param backlog Maximum length of the pending connection queue.
            Defaults to SOMAXCONN (system maximum).

        @throws std::system_error on failure.
    */
    BOOST_COROSIO_DECL
    void listen(int backlog = SOMAXCONN);

    /** Close the acceptor.

        Releases the socket resources. Any pending accept operations
        complete with operation_canceled.
    */
    BOOST_COROSIO_DECL
    void close();

    /** Check if the acceptor is open.

        @return `true` if the acceptor socket is open.
    */
    bool is_open() const noexcept;

    /** Initiate an asynchronous accept operation.

        Waits for an incoming connection and accepts it into
        the provided socket. The peer socket must be opened
        before calling this function.

        @param peer The socket to accept the connection into.
            Must be open but not connected.

        @return An awaitable that completes with system::error_code.
            Returns success (default error_code) on successful accept,
            or an error code on failure including:
            - operation_canceled: accept was cancelled
            - connection_aborted: client disconnected before accept
    */
    accept_awaitable accept(socket& peer)
    {
        return accept_awaitable(*this, peer);
    }

    /** Cancel any pending accept operations.

        Outstanding accept operations complete with operation_canceled.
    */
    BOOST_COROSIO_DECL
    void cancel();

    /** Awaitable returned by accept(). */
    struct accept_awaitable
    {
        acceptor& a_;
        socket* peer_;
        std::stop_token token_;
        mutable system::error_code ec_;

        accept_awaitable(acceptor& a, socket& peer) noexcept
            : a_(a)
            , peer_(&peer)
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        system::error_code await_resume() const noexcept
        {
            if (token_.stop_requested())
                return make_error_code(system::errc::operation_canceled);
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

private:
    friend struct accept_awaitable;

    BOOST_COROSIO_DECL
    void do_accept(
        std::coroutine_handle<>,
        capy::any_dispatcher,
        socket&,
        std::stop_token,
        system::error_code*);

    BOOST_COROSIO_DECL
    static void accept_transfer(
        void* peer_ptr,
        void* svc_ptr,
        detail::win_socket_impl* impl,
        SOCKET sock);

    capy::execution_context* ctx_;
    detail::win_socket_impl* impl_ = nullptr;
};

} // namespace tcp
} // namespace corosio
} // namespace boost

#endif
