//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tls_context.hpp>
#include "detail/context_impl.hpp"

#include <cerrno>
#include <fstream>
#include <sstream>

namespace boost::corosio {

namespace {

// Read an entire file in binary mode into `out`, returning ENOENT if it
// cannot be opened. Shared by the file-based credential/trust loaders.
std::error_code
read_file_contents(std::string_view filename, std::string& out)
{
    std::ifstream file(std::string(filename), std::ios::binary);
    if (!file)
        return std::error_code(ENOENT, std::generic_category());

    std::ostringstream ss;
    ss << file.rdbuf();
    out = ss.str();
    return {};
}

} // namespace

tls_context::tls_context() : impl_(std::make_shared<impl>()) {}

//
// Credential Loading
//

std::error_code
tls_context::use_certificate(
    std::string_view certificate, tls_file_format format)
{
    impl_->entity_certificate = std::string(certificate);
    impl_->entity_cert_format = format;
    return {};
}

std::error_code
tls_context::use_certificate_file(
    std::string_view filename, tls_file_format format)
{
    if (auto ec = read_file_contents(filename, impl_->entity_certificate); ec)
        return ec;
    impl_->entity_cert_format = format;
    return {};
}

std::error_code
tls_context::use_certificate_chain(std::string_view chain)
{
    impl_->certificate_chain = std::string(chain);
    return {};
}

std::error_code
tls_context::use_certificate_chain_file(std::string_view filename)
{
    return read_file_contents(filename, impl_->certificate_chain);
}

std::error_code
tls_context::use_private_key(
    std::string_view private_key, tls_file_format format)
{
    impl_->private_key        = std::string(private_key);
    impl_->private_key_format = format;
    return {};
}

std::error_code
tls_context::use_private_key_file(
    std::string_view filename, tls_file_format format)
{
    if (auto ec = read_file_contents(filename, impl_->private_key); ec)
        return ec;
    impl_->private_key_format = format;
    return {};
}

std::error_code
tls_context::use_pkcs12(std::string_view data, std::string_view passphrase)
{
    // assign(ptr, len) rather than std::string(string_view): libstdc++'s
    // basic_string(const char*, size_t) computes std::distance(s, s + n),
    // whose one-past-the-end pointer ASan's detect_invalid_pointer_pairs
    // rejects for a global buffer. assign copies without that subtraction.
    impl_->pkcs12_data.assign(data.data(), data.size());
    impl_->pkcs12_password.assign(passphrase.data(), passphrase.size());
    return {};
}

std::error_code
tls_context::use_pkcs12_file(
    std::string_view filename, std::string_view passphrase)
{
    if (auto ec = read_file_contents(filename, impl_->pkcs12_data); ec)
        return ec;
    // assign(ptr, len), not std::string(passphrase): see the note in
    // use_pkcs12.
    impl_->pkcs12_password.assign(passphrase.data(), passphrase.size());
    return {};
}

//
// Trust Anchors
//

std::error_code
tls_context::add_certificate_authority(std::string_view ca)
{
    impl_->ca_certificates.emplace_back(ca);
    return {};
}

std::error_code
tls_context::load_verify_file(std::string_view filename)
{
    std::string contents;
    if (auto ec = read_file_contents(filename, contents); ec)
        return ec;
    impl_->ca_certificates.push_back(std::move(contents));
    return {};
}

std::error_code
tls_context::add_verify_path(std::string_view path)
{
    impl_->verify_paths.emplace_back(path);
    return {};
}

std::error_code
tls_context::set_default_verify_paths()
{
    impl_->use_default_verify_paths = true;
    return {};
}

//
// Protocol Configuration
//

std::error_code
tls_context::set_min_protocol_version(tls_version v)
{
    impl_->min_version = v;
    return {};
}

std::error_code
tls_context::set_max_protocol_version(tls_version v)
{
    impl_->max_version = v;
    return {};
}

std::error_code
tls_context::set_ciphersuites(std::string_view ciphers)
{
    impl_->ciphersuites = std::string(ciphers);
    return {};
}

std::error_code
tls_context::set_ciphersuites_tls13(std::string_view ciphers)
{
    impl_->ciphersuites_tls13 = std::string(ciphers);
    return {};
}

std::error_code
tls_context::set_alpn(std::initializer_list<std::string_view> protocols)
{
    // Validate before mutating so a bad entry doesn't silently drop part of
    // the list (or wipe a prior valid configuration). A name must be a
    // non-empty token no longer than 255 bytes (the ALPN wire length field)
    // and must not contain a comma (WolfSSL's list separator).
    for (auto const& p : protocols)
        if (p.empty() || p.size() > 255 ||
            p.find(',') != std::string_view::npos)
            return std::make_error_code(std::errc::invalid_argument);

    impl_->alpn_protocols.clear();
    for (auto const& p : protocols)
        impl_->alpn_protocols.emplace_back(p);
    return {};
}

//
// Certificate Verification
//

std::error_code
tls_context::set_verify_mode(tls_verify_mode mode)
{
    impl_->verification_mode = mode;
    return {};
}

std::error_code
tls_context::set_verify_depth(int depth)
{
    impl_->verify_depth = depth;
    return {};
}

void
tls_context::set_hostname(std::string_view hostname)
{
    impl_->hostname = std::string(hostname);
}

void
tls_context::set_servername_callback_impl(
    std::function<bool(std::string_view)> callback)
{
    impl_->servername_callback = std::move(callback);
}

void
tls_context::set_password_callback_impl(
    std::function<std::string(std::size_t, tls_password_purpose)> callback)
{
    impl_->password_callback = std::move(callback);
}

void
tls_context::set_verify_callback_impl(
    std::function<bool(bool, verify_context&)> callback)
{
    impl_->verify_callback = std::move(callback);
}

//
// Revocation Checking
//

std::error_code
tls_context::add_crl(std::string_view crl)
{
    impl_->crls.emplace_back(crl);
    return {};
}

std::error_code
tls_context::add_crl_file(std::string_view filename)
{
    std::string contents;
    if (auto ec = read_file_contents(filename, contents); ec)
        return ec;
    impl_->crls.push_back(std::move(contents));
    return {};
}

void
tls_context::set_revocation_policy(tls_revocation_policy policy)
{
    impl_->revocation = policy;
}

} // namespace boost::corosio
