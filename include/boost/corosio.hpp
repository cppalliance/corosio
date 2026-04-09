//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_HPP
#define BOOST_COROSIO_HPP

#include <boost/corosio/backend.hpp>
#include <boost/corosio/cancel.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/file_base.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/ipv6_address.hpp>
#include <boost/corosio/local_datagram.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_socket_pair.hpp>
#include <boost/corosio/local_stream.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/random_access_file.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/resolver_results.hpp>
#include <boost/corosio/signal_set.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/stream_file.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_server.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/corosio/udp.hpp>
#include <boost/corosio/udp_socket.hpp>

#include <boost/corosio/tls_context.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/tls_stream.hpp>
#include <boost/corosio/wolfssl_stream.hpp>

#endif
