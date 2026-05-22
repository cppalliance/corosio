//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

/** @file native.hpp

    Include all native (devirtualized) public headers:
    I/O context, sockets, acceptor, resolver, signal set,
    timer, and cancellation helpers.
*/

#ifndef BOOST_COROSIO_NATIVE_NATIVE_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_HPP

#include <boost/corosio/native/native_cancel.hpp>
#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/native/native_local_datagram_socket.hpp>
#include <boost/corosio/native/native_local_stream_acceptor.hpp>
#include <boost/corosio/native/native_local_stream_socket.hpp>
#include <boost/corosio/native/native_random_access_file.hpp>
#include <boost/corosio/native/native_resolver.hpp>
#include <boost/corosio/native/native_signal_set.hpp>
#include <boost/corosio/native/native_stream_file.hpp>
#include <boost/corosio/native/native_tcp_acceptor.hpp>
#include <boost/corosio/native/native_tcp_socket.hpp>
#include <boost/corosio/native/native_timer.hpp>
#include <boost/corosio/native/native_udp_socket.hpp>

#endif
