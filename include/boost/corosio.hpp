//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_HPP
#define BOOST_COROSIO_HPP

/** @file
    @brief Master include file for Boost.Corosio

    This header includes all public headers of the Boost.Corosio library.
    Including this file provides access to:

    - @ref boost::corosio::io_context "io_context" — Event loop for async operations
    - @ref boost::corosio::socket "socket" — Asynchronous TCP socket
    - @ref boost::corosio::endpoint "endpoint" — IP endpoint supporting IPv4 and IPv6
    - @ref boost::corosio::tls_stream "tls_stream" — TLS stream adapter

    @par Example
    @code
    #include <boost/corosio.hpp>
    #include <boost/capy/task.hpp>
    #include <boost/capy/async_run.hpp>

    namespace corosio = boost::corosio;
    namespace capy = boost::capy;

    capy::task<void> client(corosio::io_context& ioc)
    {
        corosio::socket s(ioc);
        s.open();
        co_await s.connect(corosio::endpoint(
            boost::urls::ipv4_address::loopback(), 8080));
    }
    @endcode
*/

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/socket.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/tls_stream.hpp>

#endif
