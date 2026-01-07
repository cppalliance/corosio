//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <capy/service_provider.hpp>

#include <cassert>
#include <iostream>

//------------------------------------------------
// Example services

struct file_service : capy::service
{
    virtual int read() = 0;
};

struct posix_file_service : file_service
{
    using key_type = file_service;

    explicit posix_file_service(capy::service_provider&)
    {
        std::cout << "posix_file_service created\n";
    }

    void shutdown() override { std::cout << "posix_file_service stopped\n"; }

    int read() override { return 42; }
};

struct timer_service : capy::service
{
    explicit timer_service(capy::service_provider&) { std::cout << "timer_service created\n"; }

    void shutdown() override { std::cout << "timer_service stopped\n"; }

    void schedule() {}
};

struct resolver_service : capy::service
{
    explicit resolver_service(capy::service_provider&, int port) : port_(port)
    {
        std::cout << "resolver_service created with port " << port << "\n";
    }

    void shutdown() override { std::cout << "resolver_service stopped\n"; }

    int port_;
};

//------------------------------------------------
// Example io_context

class io_context : public capy::service_provider
{
public:
};

//------------------------------------------------

int main()
{
    std::cout << "=== Test 1: Basic service creation ===\n";
    {
        io_context ctx;

        auto& timer = ctx.make_service<timer_service>();
        timer.schedule();

        assert(ctx.has_service<timer_service>());
        assert(ctx.find_service<timer_service>() == &timer);
    }

    std::cout << "\n=== Test 2: key_type lookup ===\n";
    {
        io_context ctx;

        auto& posix = ctx.make_service<posix_file_service>();
        (void)posix;

        // Can find by concrete type
        assert(ctx.find_service<posix_file_service>() == &posix);

        // Can find by key_type (base class)
        assert(ctx.find_service<file_service>() == &posix);

        // Verify it's the same object
        assert(ctx.find_service<file_service>()->read() == 42);
    }

    std::cout << "\n=== Test 3: use_service (get or create) ===\n";
    {
        io_context ctx;

        // First call creates
        auto& t1 = ctx.use_service<timer_service>();
        (void)t1;

        // Second call returns same instance
        auto& t2 = ctx.use_service<timer_service>();
        (void)t2;

        assert(&t1 == &t2);
    }

    std::cout << "\n=== Test 4: make_service with extra args ===\n";
    {
        io_context ctx;

        auto& resolver = ctx.make_service<resolver_service>(8080);
        (void)resolver;
        assert(resolver.port_ == 8080);
    }

    std::cout << "\n=== Test 5: Stop order (reverse) ===\n";
    {
        io_context ctx;

        ctx.make_service<timer_service>();
        ctx.make_service<posix_file_service>();
        ctx.make_service<resolver_service>(53);

        std::cout << "Destroying context...\n";
        // Destructor will call do_shutdown()
        // Should see: resolver, posix_file, timer (reverse order)
    }

    std::cout << "\n=== Test 6: Duplicate service throws ===\n";
    {
        io_context ctx;

        ctx.make_service<timer_service>();

        bool threw = false;
        try
        {
            ctx.make_service<timer_service>();
        }
        catch(std::invalid_argument const& e)
        {
            threw = true;
            std::cout << "Caught expected exception: " << e.what() << "\n";
        }
        assert(threw && "Expected duplicate service to throw");
        (void)threw; // Suppress unused variable warning
    }

    std::cout << "\n=== All tests passed! ===\n";
    return 0;
}

