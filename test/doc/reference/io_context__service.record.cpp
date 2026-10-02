//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into the documentation for
// io_context::service by doc/addons/extensions/reference-snippets.lua.
// The symbol is inherited from capy::execution_context (declared in
// boost/capy/ex/execution_context.hpp); MrDocs lists it under io_context,
// so the inherited docstring's example marker resolves against this file.
// The tagged region is what the reference renders; scaffolding stays
// outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/io_context.hpp>

#include <tuple>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

namespace ex_1 {
// tag::example[]
struct my_service : capy::execution_context::service
{
    explicit my_service(capy::execution_context&) {}

protected:
    void shutdown() override
    {
        // Cancel pending operations, release resources
    }
};

void register_service()
{
    corosio::io_context ioc;

    // Created on first use, owned by the context for its lifetime
    auto& svc = ioc.use_service<my_service>();
    std::ignore = svc;
}
// end::example[]
} // namespace ex_1

} // namespace
