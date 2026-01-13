//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_WOLFSSL_STREAM_HPP
#define BOOST_COROSIO_WOLFSSL_STREAM_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io_stream.hpp>

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace boost {
namespace corosio {

/** A TLS stream using WolfSSL.

    This class wraps an underlying stream derived from @ref io_stream
    and provides TLS encryption using the WolfSSL library.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe.

    @par Example
    @code
    corosio::socket raw_socket(ioc);
    raw_socket.open();
    co_await raw_socket.connect(endpoint);

    corosio::wolfssl_stream secure(std::move(raw_socket));
    // Use secure stream for TLS communication
    @endcode
*/
class wolfssl_stream : public io_stream
{
public:
    /** Construct a WolfSSL stream wrapping an existing stream.

        The stream argument must be a type unambiguously derived from
        @ref io_stream. The stream is moved into internal storage.

        @param stream The underlying stream to wrap. Must be derived
            from io_stream.

        @tparam Stream A type derived from io_stream.
    */
    template<class Stream>
        requires std::derived_from<std::remove_cvref_t<Stream>, io_stream> &&
                 (!std::same_as<std::remove_cvref_t<Stream>, wolfssl_stream>)
    explicit
    wolfssl_stream(Stream&& stream)
        : s_(std::make_unique<std::remove_cvref_t<Stream>>(
              std::forward<Stream>(stream)))
    {
        construct();
    }

private:
    BOOST_COROSIO_DECL void construct();

    struct wolfssl_stream_impl;

    std::unique_ptr<io_stream> s_;
};

} // namespace corosio
} // namespace boost

#endif
