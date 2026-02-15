//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP
#include "src/detail/iocp/sockets.hpp"
#else
#include "src/detail/socket_service.hpp"
#endif

#include <boost/corosio/detail/except.hpp>

namespace boost::corosio {

tcp_acceptor::
~tcp_acceptor()
{
    close();
}

tcp_acceptor::
tcp_acceptor(
    capy::execution_context& ctx)
#if BOOST_COROSIO_HAS_IOCP
    : io_object(create_handle<detail::win_acceptor_service>(ctx))
#else
    : io_object(create_handle<detail::acceptor_service>(ctx))
#endif
{
}

std::error_code
tcp_acceptor::
listen(endpoint ep, int backlog)
{
    if (is_open())
        close();

#if BOOST_COROSIO_HAS_IOCP
    auto& svc = static_cast<detail::win_acceptor_service&>(h_.service());
#else
    auto& svc = static_cast<detail::acceptor_service&>(h_.service());
#endif
    return svc.open_acceptor(
        *static_cast<tcp_acceptor::implementation*>(h_.get()), ep, backlog);
}

void
tcp_acceptor::
close()
{
    if (!is_open())
        return;
    h_.service().close(h_);
}

void
tcp_acceptor::
cancel()
{
    if (!is_open())
        return;
    get().cancel();
}

endpoint
tcp_acceptor::
local_endpoint() const noexcept
{
    if (!is_open())
        return endpoint{};
    return get().local_endpoint();
}

} // namespace boost::corosio
