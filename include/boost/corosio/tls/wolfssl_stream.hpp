//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TLS_WOLFSSL_STREAM_HPP
#define BOOST_COROSIO_TLS_WOLFSSL_STREAM_HPP

#if !defined(BOOST_COROSIO_SOURCE) && defined(BOOST_COROSIO_USE_MODULES)
import boost.corosio;
#else

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/tls/context.hpp>
#include <boost/corosio/tls/tls_stream.hpp>

namespace boost {
namespace corosio {

/** A TLS stream using WolfSSL.

    This class wraps an underlying stream derived from @ref io_stream
    and provides TLS encryption using the WolfSSL library.

    Inherits handshake(), shutdown(), read_some(), and write_some()
    from @ref tls_stream.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe.

    @par Example
    @code
    tls::context ctx;
    ctx.set_hostname( "example.com" );
    ctx.set_verify_mode( tls::verify_mode::peer );

    corosio::socket raw_socket( ioc );
    raw_socket.open();
    co_await raw_socket.connect( endpoint );

    corosio::wolfssl_stream secure( raw_socket, ctx );
    co_await secure.handshake( wolfssl_stream::client );
    // Use secure stream for TLS communication
    @endcode
*/
class BOOST_COROSIO_DECL
    wolfssl_stream : public tls_stream
{
public:
    /** Construct a WolfSSL stream.

        The underlying stream must remain valid for the lifetime of
        this wolfssl_stream object. The context's configuration is
        captured; subsequent modifications to the context are not
        reflected in this stream.

        @param stream Reference to the underlying stream to wrap.
        @param ctx The TLS context containing configuration.
    */
    wolfssl_stream(io_stream& stream, tls::context ctx);

    /** Destructor.

        Releases the underlying WolfSSL resources.
    */
    ~wolfssl_stream();
};

} // namespace corosio
} // namespace boost

#endif
#endif
