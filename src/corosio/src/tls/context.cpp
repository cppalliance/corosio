//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tls/context.hpp>
#include "detail/context_impl.hpp"

#include <cerrno>
#include <fstream>
#include <sstream>

namespace boost::corosio::tls {

//------------------------------------------------------------------------------

context::
context()
    : impl_( std::make_shared<impl>() )
{
}

//------------------------------------------------------------------------------
//
// Credential Loading
//
//------------------------------------------------------------------------------

system::result<void>
context::
use_certificate(
    std::string_view certificate,
    file_format format )
{
    impl_->entity_certificate = std::string( certificate );
    impl_->entity_cert_format = format;
    return {};
}

system::result<void>
context::
use_certificate_file(
    std::string_view filename,
    file_format format )
{
    std::ifstream file( std::string( filename ), std::ios::binary );
    if( !file )
        return system::error_code( ENOENT, system::generic_category() );

    std::ostringstream ss;
    ss << file.rdbuf();
    impl_->entity_certificate = ss.str();
    impl_->entity_cert_format = format;
    return {};
}

system::result<void>
context::
use_certificate_chain( std::string_view chain )
{
    impl_->certificate_chain = std::string( chain );
    return {};
}

system::result<void>
context::
use_certificate_chain_file( std::string_view filename )
{
    std::ifstream file( std::string( filename ), std::ios::binary );
    if( !file )
        return system::error_code( ENOENT, system::generic_category() );

    std::ostringstream ss;
    ss << file.rdbuf();
    impl_->certificate_chain = ss.str();
    return {};
}

system::result<void>
context::
use_private_key(
    std::string_view private_key,
    file_format format )
{
    impl_->private_key = std::string( private_key );
    impl_->private_key_format = format;
    return {};
}

system::result<void>
context::
use_private_key_file(
    std::string_view filename,
    file_format format )
{
    std::ifstream file( std::string( filename ), std::ios::binary );
    if( !file )
        return system::error_code( ENOENT, system::generic_category() );

    std::ostringstream ss;
    ss << file.rdbuf();
    impl_->private_key = ss.str();
    impl_->private_key_format = format;
    return {};
}

system::result<void>
context::
use_pkcs12(
    std::string_view /*data*/,
    std::string_view /*passphrase*/ )
{
    // TODO: Implement PKCS#12 parsing
    return system::error_code( ENOTSUP, system::generic_category() );
}

system::result<void>
context::
use_pkcs12_file(
    std::string_view /*filename*/,
    std::string_view /*passphrase*/ )
{
    // TODO: Implement PKCS#12 file loading
    return system::error_code( ENOTSUP, system::generic_category() );
}

//------------------------------------------------------------------------------
//
// Trust Anchors
//
//------------------------------------------------------------------------------

system::result<void>
context::
add_certificate_authority( std::string_view ca )
{
    impl_->ca_certificates.emplace_back( ca );
    return {};
}

system::result<void>
context::
load_verify_file( std::string_view filename )
{
    std::ifstream file( std::string( filename ), std::ios::binary );
    if( !file )
        return system::error_code( ENOENT, system::generic_category() );

    std::ostringstream ss;
    ss << file.rdbuf();
    impl_->ca_certificates.push_back( ss.str() );
    return {};
}

system::result<void>
context::
add_verify_path( std::string_view path )
{
    impl_->verify_paths.emplace_back( path );
    return {};
}

system::result<void>
context::
set_default_verify_paths()
{
    impl_->use_default_verify_paths = true;
    return {};
}

//------------------------------------------------------------------------------
//
// Protocol Configuration
//
//------------------------------------------------------------------------------

system::result<void>
context::
set_min_protocol_version( version v )
{
    impl_->min_version = v;
    return {};
}

system::result<void>
context::
set_max_protocol_version( version v )
{
    impl_->max_version = v;
    return {};
}

system::result<void>
context::
set_ciphersuites( std::string_view ciphers )
{
    impl_->ciphersuites = std::string( ciphers );
    return {};
}

system::result<void>
context::
set_alpn( std::initializer_list<std::string_view> protocols )
{
    impl_->alpn_protocols.clear();
    for( auto const& p : protocols )
        impl_->alpn_protocols.emplace_back( p );
    return {};
}

//------------------------------------------------------------------------------
//
// Certificate Verification
//
//------------------------------------------------------------------------------

system::result<void>
context::
set_verify_mode( verify_mode mode )
{
    impl_->verification_mode = mode;
    return {};
}

system::result<void>
context::
set_verify_depth( int depth )
{
    impl_->verify_depth = depth;
    return {};
}

void
context::
set_hostname( std::string_view hostname )
{
    impl_->hostname = std::string( hostname );
}

void
context::
set_servername_callback_impl(
    std::function<bool( std::string_view )> callback )
{
    impl_->servername_callback = std::move( callback );
}

//------------------------------------------------------------------------------
//
// Revocation Checking
//
//------------------------------------------------------------------------------

system::result<void>
context::
add_crl( std::string_view crl )
{
    impl_->crls.emplace_back( crl );
    return {};
}

system::result<void>
context::
add_crl_file( std::string_view filename )
{
    std::ifstream file( std::string( filename ), std::ios::binary );
    if( !file )
        return system::error_code( ENOENT, system::generic_category() );

    std::ostringstream ss;
    ss << file.rdbuf();
    impl_->crls.push_back( ss.str() );
    return {};
}

system::result<void>
context::
set_ocsp_staple( std::string_view response )
{
    impl_->ocsp_staple = std::string( response );
    return {};
}

void
context::
set_require_ocsp_staple( bool require )
{
    impl_->require_ocsp_staple = require;
}

void
context::
set_revocation_policy( revocation_policy policy )
{
    impl_->revocation = policy;
}

} // namespace boost::corosio::tls
