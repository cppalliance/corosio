//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/io_context.hpp's
// documentation for io_context_options, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/io_context.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::configure[]
void configure_for_high_throughput()
{
    corosio::io_context_options opts;

    // Larger epoll_wait()/kevent() batches trade per-connection fairness
    // for fewer syscalls under sustained load.
    opts.max_events_per_poll = 256;

    // Raises the ceiling on adaptive inline-completion ramp-up: the
    // budget still starts small and doubles each fully-consumed cycle,
    // capped here instead of at the smaller default. On a multi-threaded
    // context (concurrency_hint > 1), touching any budget field like
    // this one also opts out of the library's default of disabling
    // inline completion entirely for cross-thread work-stealing.
    opts.inline_budget_max = 32;

    // More worker threads for blocking file I/O and DNS resolution.
    opts.thread_pool_size = 4;

    corosio::io_context ioc(opts);
}
// end::configure[]

} // namespace
