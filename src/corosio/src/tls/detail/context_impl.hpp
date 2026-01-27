//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef SRC_TLS_DETAIL_CONTEXT_IMPL_HPP
#define SRC_TLS_DETAIL_CONTEXT_IMPL_HPP

#include <boost/corosio/tls/context.hpp>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace boost::corosio::tls {

namespace detail {

/** Abstract base for cached native SSL contexts.

    Stored in context::impl as an intrusive linked list.
    Each TLS backend derives from this to cache its native
    context handle ( WOLFSSL_CTX*, SSL_CTX*, etc. ).
*/
class native_context_base
{
public:
    native_context_base* next_ = nullptr;
    void const* service_ = nullptr;

    virtual ~native_context_base() = default;
};

struct context_data
{
    //--------------------------------------------
    // Credentials

    std::string entity_certificate;
    file_format entity_cert_format = file_format::pem;
    std::string certificate_chain;
    std::string private_key;
    file_format private_key_format = file_format::pem;

    //--------------------------------------------
    // Trust anchors

    std::vector<std::string> ca_certificates;
    std::vector<std::string> verify_paths;
    bool use_default_verify_paths = false;

    //--------------------------------------------
    // Protocol settings

    version min_version = version::tls_1_2;
    version max_version = version::tls_1_3;
    std::string ciphersuites;
    std::vector<std::string> alpn_protocols;

    //--------------------------------------------
    // Verification

    verify_mode verification_mode = verify_mode::none;
    int verify_depth = 100;
    std::string hostname;
    std::function<bool( bool, void* )> verify_callback;

    //--------------------------------------------
    // SNI (Server Name Indication)

    std::function<bool( std::string_view )> servername_callback;

    //--------------------------------------------
    // Revocation

    std::vector<std::string> crls;
    std::string ocsp_staple;
    bool require_ocsp_staple = false;
    revocation_policy revocation = revocation_policy::disabled;

    //--------------------------------------------
    // Password

    std::function<std::string( std::size_t, password_purpose )> password_callback;

    //--------------------------------------------
    // Cached native contexts (intrusive list)

    mutable std::mutex native_contexts_mutex_;
    mutable native_context_base* native_contexts_ = nullptr;

    /** Find or insert a cached native context.

        @param service The unique key for the backend.
        @param create Factory function called if not found.

        @return Pointer to the cached native context.
    */
    template<typename Factory>
    native_context_base*
    find( void const* service, Factory&& create ) const
    {
        std::lock_guard<std::mutex> lock( native_contexts_mutex_ );

        for( auto* p = native_contexts_; p; p = p->next_ )
            if( p->service_ == service )
                return p;

        // Not found - create and prepend
        auto* ctx = create();
        ctx->service_ = service;
        ctx->next_ = native_contexts_;
        native_contexts_ = ctx;
        return ctx;
    }

    ~context_data()
    {
        // Clean up cached native contexts (no lock needed - destructor)
        while( native_contexts_ )
        {
            auto* next = native_contexts_->next_;
            delete native_contexts_;
            native_contexts_ = next;
        }
    }
};

} // namespace detail

//------------------------------------------------------------------------------

/** Implementation of tls::context.

    Contains all portable TLS configuration data plus
    cached native SSL contexts as an intrusive list.
*/
struct context::impl : detail::context_data
{
};

//------------------------------------------------------------------------------

namespace detail {

/** Return the TLS context data.

    Provides read-only access to the portable configuration
    stored in the context.

    @param ctx The TLS context.

    @return Reference to the context implementation.
*/
inline context_data const&
get_context_data( context const& ctx ) noexcept
{
    return *ctx.impl_;
}

} // namespace detail

} // namespace boost::corosio::tls

#endif
