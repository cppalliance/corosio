//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef ASIO_BENCH_UNIX_SOCKET_UTILS_HPP
#define ASIO_BENCH_UNIX_SOCKET_UTILS_HPP

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/local/connect_pair.hpp>

#include <utility>

namespace asio_bench {

namespace asio = boost::asio;

using executor_type  = asio::io_context::executor_type;
using local_protocol = asio::local::stream_protocol;
using local_socket   = asio::basic_stream_socket<local_protocol, executor_type>;

/** Create a connected pair of Unix domain sockets for benchmarking. */
inline std::pair<local_socket, local_socket>
make_local_socket_pair(asio::io_context& ioc)
{
    local_socket s1(ioc.get_executor());
    local_socket s2(ioc.get_executor());
    asio::local::connect_pair(s1, s2);
    return {std::move(s1), std::move(s2)};
}

} // namespace asio_bench

#endif
