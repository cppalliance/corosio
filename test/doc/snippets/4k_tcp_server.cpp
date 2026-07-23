//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4k.tcp-server.adoc.

// Fragments deliberately leave results and bindings unused; the pages
// explain the values in prose instead.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-lambda-capture"
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4834) // discarding [[nodiscard]] return value
#pragma warning(disable: 4189) // local variable initialized but not referenced
#pragma warning(disable: 4100) // unreferenced formal parameter
#pragma warning(disable: 4101) // unreferenced local variable
#pragma warning(disable: 4456) // declaration hides previous local declaration
#pragma warning(disable: 4457) // declaration hides function parameter
#pragma warning(disable: 4458) // declaration hides class member
#pragma warning(disable: 4459) // declaration hides global declaration
#endif

// tag::assume[]
#include <boost/corosio/tcp_server.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::assume[]

#include <boost/capy/buffers.hpp>
#include <boost/capy/write.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "test_suite.hpp"

namespace {

// tag::overview[]
class echo_worker : public corosio::tcp_server::worker_base
{
    corosio::io_context& ctx_;
    corosio::tcp_socket sock_;
    std::string buf;

public:
    explicit echo_worker(corosio::io_context& ctx)
        : ctx_(ctx)
        , sock_(ctx)
    {
        buf.reserve(4096);
    }

    corosio::tcp_socket& socket() override { return sock_; }

    void run(corosio::tcp_server::launcher launch) override
    {
        launch(ctx_.get_executor(), do_echo());
    }

    capy::task<void> do_echo();
};

// Build the worker pool as a range of pointer-like objects.
auto make_echo_workers(corosio::io_context& ctx, int n)
{
    std::vector<std::unique_ptr<corosio::tcp_server::worker_base>> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i)
        v.push_back(std::make_unique<echo_worker>(ctx));
    return v;
}

class echo_server : public corosio::tcp_server
{
public:
    echo_server(corosio::io_context& ioc, int max_workers)
        : tcp_server(ioc, ioc.get_executor())
    {
        set_workers(make_echo_workers(ioc, max_workers));
    }
};
// end::overview[]

capy::task<void> echo_worker::do_echo()
{
    co_return;
}

// Abridged interface listing; the real class lives in
// <boost/corosio/tcp_server.hpp>, where `launcher` is a sibling
// member of the enclosing tcp_server and needs no qualification.
namespace worker_base_synopsis {
using launcher = corosio::tcp_server::launcher;
// tag::worker_base[]
class worker_base
{
    // Private list/bookkeeping members managed by tcp_server.
public:
    worker_base();
    virtual ~worker_base();

    virtual void run(launcher launch) = 0;
    virtual corosio::tcp_socket& socket() = 0;
};
// end::worker_base[]
} // namespace worker_base_synopsis

// tag::my_worker[]
class my_worker : public corosio::tcp_server::worker_base
{
    corosio::io_context& ctx_;
    corosio::tcp_socket sock_;
    std::string request_buf;
    std::string response_buf;

public:
    explicit my_worker(corosio::io_context& ctx)
        : ctx_(ctx)
        , sock_(ctx)
    {}

    corosio::tcp_socket& socket() override { return sock_; }

    void run(corosio::tcp_server::launcher launch) override
    {
        // tag::launch_call[]
        launch(ctx_.get_executor(), handle_connection());
        // end::launch_call[]
    }

    capy::task<void> handle_connection()
    {
        // Handle the connection using sock_
        // Worker is automatically returned to pool when coroutine ends
        co_return;
    }
};
// end::my_worker[]

// Abridged member signature; the real declaration lives in
// <boost/corosio/tcp_server.hpp>.
namespace set_workers_synopsis {
// tag::set_workers_signature[]
template<class Range>
void set_workers(Range&& workers);
// end::set_workers_signature[]
} // namespace set_workers_synopsis

// tag::make_workers[]
auto make_workers(corosio::io_context& ctx, int n)
{
    std::vector<std::unique_ptr<corosio::tcp_server::worker_base>> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i)
        v.push_back(std::make_unique<my_worker>(ctx));
    return v;
}

class my_server : public corosio::tcp_server
{
public:
    my_server(corosio::io_context& ioc, int max_workers)
        : tcp_server(ioc, ioc.get_executor())
    {
        set_workers(make_workers(ioc, max_workers));
    }
};
// end::make_workers[]

class launcher_demo : public corosio::tcp_server::worker_base
{
    corosio::tcp_socket sock_;
    corosio::io_context::executor_type executor;

public:
    explicit launcher_demo(corosio::io_context& ctx)
        : sock_(ctx)
        , executor(ctx.get_executor())
    {}

    corosio::tcp_socket& socket() override { return sock_; }

    capy::task<void> my_coroutine() { co_return; }

    // tag::launcher_run[]
    void run(corosio::tcp_server::launcher launch) override
    {
        // Create and launch the session coroutine
        launch(executor, my_coroutine());
    }
    // end::launcher_run[]
};

// Abridged member signature of tcp_server::launcher's call operator.
namespace launcher_synopsis {
struct launcher
{
// tag::launcher_signature[]
template<class Executor>
void operator()(Executor const& ex, capy::task<void> task);
// end::launcher_signature[]
};
} // namespace launcher_synopsis

// Fragments that bind privileged or fixed ports are compiled but
// never executed.
[[maybe_unused]] void
bind_one(corosio::tcp_server& server)
{
    // tag::bind[]
    auto ec = server.bind(corosio::endpoint(8080));
    if (ec)
        std::cerr << "Bind failed: " << ec.message() << "\n";
    // end::bind[]
}

[[maybe_unused]] void
bind_many(corosio::tcp_server& server)
{
    // tag::bind_many[]
    server.bind(corosio::endpoint(80));
    server.bind(corosio::endpoint(443));
    // end::bind_many[]
}

[[maybe_unused]] void
start_server(corosio::tcp_server& server)
{
    // tag::start[]
    server.start();
    // end::start[]
}

[[maybe_unused]] void
multiple_ports(corosio::tcp_server& server)
{
    // tag::multiple_ports[]
    server.bind(corosio::endpoint(80));    // HTTP
    server.bind(corosio::endpoint(443));   // HTTPS
    server.start();
    // end::multiple_ports[]
}

struct reuse_worker
{
    std::string request_;
    std::string response_;

    // tag::worker_reuse[]
    capy::task<void> do_session()
    {
        // Reset state at session start
        request_.clear();
        response_.clear();

        // ... handle connection ...
    // end::worker_reuse[]
        co_return;
    // tag::worker_reuse[]

        // Socket closed, worker returns to pool
    }
    // end::worker_reuse[]
};

struct tcp_server_test
{
    void
    testEchoServer()
    {
        // Constructing the server installs the worker pool; no port
        // is bound and the accept loop never starts.
        corosio::io_context ioc;
        echo_server server(ioc, 4);
        BOOST_TEST(server.local_endpoint().port() == 0);
    }

    void
    testMyServer()
    {
        corosio::io_context ioc;
        my_server server(ioc, 4);
        BOOST_TEST(server.local_endpoint().port() == 0);
    }

    void
    run()
    {
        testEchoServer();
        testMyServer();
    }
};

} // namespace

TEST_SUITE(tcp_server_test, "boost.corosio.doc.4k_tcp_server");
