//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into
// include/boost/corosio/local_stream_acceptor.hpp's documentation for
// local_stream_acceptor, by doc/addons/extensions/reference-snippets.lua.
// The tagged region is what the reference renders; scaffolding stays
// outside the tags.
//
// local_stream_acceptor is not gated behind BOOST_COROSIO_POSIX -- it has a
// Windows (IOCP) backend (native/detail/iocp/win_local_stream_acceptor.hpp),
// and bind_option::unlink_existing is handled on both POSIX (::unlink) and
// Windows (::DeleteFileA) in local_stream_acceptor.cpp, so this region needs
// no platform guard.

#include "../doc_warnings.hpp"

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>

#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

// tag::bind_listen_accept[]
capy::task<> bind_listen_accept(corosio::io_context& ioc)
{
    corosio::local_stream_acceptor acc(ioc);
    if (auto ec = acc.open())
        co_return;
    if (auto ec = acc.bind(corosio::local_endpoint("/tmp/my_app.sock"),
                           corosio::bind_option::unlink_existing))
        co_return;
    if (auto ec = acc.listen())
        co_return;

    auto [aec, peer] = co_await acc.accept();
    if (aec)
        co_return;
}
// end::bind_listen_accept[]

} // namespace
