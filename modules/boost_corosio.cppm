module;

#include <boost/corosio.hpp>

export module boost.corosio;

export namespace boost::corosio {

using corosio::acceptor;
using corosio::endpoint;
using corosio::io_buffer_param;
using corosio::io_context;
using corosio::io_object;
using corosio::io_stream;
using corosio::read;
using corosio::resolve_flags;
using corosio::resolver;
using corosio::resolver_entry;
using corosio::resolver_results;
using corosio::signal_set;
using corosio::socket;
using corosio::tcp_server;
using corosio::timer;
using corosio::write;
using corosio::operator|;
using corosio::operator&;
using corosio::operator&=;
using corosio::operator|=;

// I think this should be in tls
using corosio::tls_stream;
using corosio::openssl_stream;
using corosio::wolfssl_stream;

namespace tls {
using corosio::tls::context;
using corosio::tls::file_format;
using corosio::tls::password_purpose;
using corosio::tls::revocation_policy;
using corosio::tls::role;
using corosio::tls::verify_mode;
using corosio::tls::version;
} // namespace tls


} // namespace boost::corosio


