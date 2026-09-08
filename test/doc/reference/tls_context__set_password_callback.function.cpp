//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::set_password_callback, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

// The example reads the passphrase with std::getenv, which the Windows CRT
// deprecates and this directory builds with warnings as errors.
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <boost/corosio/tls_context.hpp>

#include <cstddef>
#include <cstdlib>
#include <string>

namespace corosio = boost::corosio;

namespace {

// tag::set_password_callback[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void
supply_the_key_passphrase(corosio::tls_context& ctx)
{
    // Keep the passphrase out of the source and out of the shipped binary:
    // read it from the environment, a secrets service, or a prompt. There is
    // no error code to check -- the callback is recorded here and runs when
    // the key material is decoded, at stream creation, so installing it
    // before the load below is a habit that keeps the pair legible rather
    // than a requirement.
    ctx.set_password_callback(
        [](std::size_t max_length, corosio::tls_password_purpose purpose) {
            // purpose distinguishes decrypting existing key material from
            // encrypting new material; a context that only loads keys sees
            // tls_password_purpose::for_reading.
            char const* pw = std::getenv("TLS_KEY_PASSPHRASE");
            if (pw == nullptr)
                return std::string(); // nothing to offer; the load fails

            // A backend may copy at most max_length bytes, so returning a
            // longer string can silently truncate it into a different, wrong
            // passphrase. Return nothing instead and let the failure be what
            // it is rather than a corrupt-key error.
            std::string passphrase(pw);
            if (passphrase.size() > max_length)
                return std::string();

            return passphrase;
        });

    if (auto ec = ctx.use_private_key_file(
            "encrypted.key", corosio::tls_file_format::pem))
        return; // report the error
}
// end::set_password_callback[]

} // namespace
