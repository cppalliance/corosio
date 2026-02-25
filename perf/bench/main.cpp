//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "corosio/benchmarks.hpp"

#ifdef BOOST_COROSIO_BENCH_HAS_ASIO
#include "asio/coroutine/benchmarks.hpp"
#include "asio/callback/benchmarks.hpp"
#endif

#include <boost/corosio/io_context.hpp>

#include <cstdlib>
#include <cstring>
#include <iostream>

#include "../common/backend_selection.hpp"
#include "common/suite.hpp"

namespace {

void
print_usage(char const* program_name)
{
    std::cout << "Usage: " << program_name << " [OPTIONS]\n\n";
    std::cout << "Options:\n";
    std::cout
        << "  --library <name>    Library to benchmark (default: corosio)\n";
    std::cout << "  --backend <name>    Select I/O backend (default: platform "
                 "default)\n";
    std::cout
        << "  --category <name>   Run only the specified benchmark category\n";
    std::cout << "  --bench <name>      Run only the specified benchmark "
                 "(prefix match)\n";
    std::cout << "  --duration <secs>   Duration per benchmark in seconds "
                 "(default: 3.0)\n";
    std::cout << "  --output <file>     Write JSON results to file\n";
    std::cout << "  --enable-microbenchmarks\n";
    std::cout
        << "                      Include microbenchmarks in 'all' runs\n";
    std::cout << "  --list              List available benchmarks\n";
    std::cout << "  --help              Show this help message\n";
    std::cout << "\n";
    std::cout << "Libraries (--library):\n";
    std::cout << "  corosio             Boost.Corosio benchmarks (default)\n";
#ifdef BOOST_COROSIO_BENCH_HAS_ASIO
    std::cout << "  asio                Boost.Asio coroutine benchmarks\n";
    std::cout << "  asio_callback       Boost.Asio callback benchmarks\n";
    std::cout << "  all                 Run all libraries\n";
#else
    std::cout
        << "  asio                (not available — Boost.Asio not found)\n";
    std::cout
        << "  asio_callback       (not available — Boost.Asio not found)\n";
    std::cout
        << "  all                 (not available — Boost.Asio not found)\n";
#endif
    std::cout << "\nUse --list to see available categories and benchmarks.\n\n";
    perf::print_available_backends();
}

template<class BackendTag>
void
add_corosio_suites(bench::benchmark_runner& runner, BackendTag)
{
    runner.add_suite("corosio", corosio_bench::make_io_context_suite<BackendTag{}>());
    runner.add_suite("corosio", corosio_bench::make_socket_throughput_suite<BackendTag{}>());
    runner.add_suite("corosio", corosio_bench::make_socket_latency_suite<BackendTag{}>());
    runner.add_suite("corosio", corosio_bench::make_http_server_suite<BackendTag{}>());
    runner.add_suite("corosio", corosio_bench::make_timer_suite<BackendTag{}>());
    runner.add_suite("corosio", corosio_bench::make_accept_churn_suite<BackendTag{}>());
    runner.add_suite("corosio", corosio_bench::make_fan_out_suite<BackendTag{}>());
}

#ifdef BOOST_COROSIO_BENCH_HAS_ASIO
void
add_asio_suites(bench::benchmark_runner& runner)
{
    runner.add_suite("asio", asio_bench::make_io_context_suite());
    runner.add_suite("asio", asio_bench::make_socket_throughput_suite());
    runner.add_suite("asio", asio_bench::make_socket_latency_suite());
    runner.add_suite("asio", asio_bench::make_http_server_suite());
    runner.add_suite("asio", asio_bench::make_timer_suite());
    runner.add_suite("asio", asio_bench::make_accept_churn_suite());
    runner.add_suite("asio", asio_bench::make_fan_out_suite());
}

void
add_asio_callback_suites(bench::benchmark_runner& runner)
{
    runner.add_suite("asio_callback", asio_callback_bench::make_io_context_suite());
    runner.add_suite("asio_callback", asio_callback_bench::make_socket_throughput_suite());
    runner.add_suite("asio_callback", asio_callback_bench::make_socket_latency_suite());
    runner.add_suite("asio_callback", asio_callback_bench::make_http_server_suite());
    runner.add_suite("asio_callback", asio_callback_bench::make_timer_suite());
    runner.add_suite("asio_callback", asio_callback_bench::make_accept_churn_suite());
    runner.add_suite("asio_callback", asio_callback_bench::make_fan_out_suite());
}
#endif

} // anonymous namespace

int
main(int argc, char* argv[])
{
    char const* library         = "corosio";
    char const* backend         = nullptr;
    char const* output_file     = nullptr;
    char const* category_filter = nullptr;
    char const* bench_filter    = nullptr;
    double duration_s           = 3.0;
    bool enable_microbenchmark  = false;
    bool list_mode              = false;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--library") == 0)
        {
            if (i + 1 < argc)
            {
                library = argv[++i];
            }
            else
            {
                std::cerr << "Error: --library requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--backend") == 0)
        {
            if (i + 1 < argc)
            {
                backend = argv[++i];
            }
            else
            {
                std::cerr << "Error: --backend requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--category") == 0)
        {
            if (i + 1 < argc)
            {
                category_filter = argv[++i];
            }
            else
            {
                std::cerr << "Error: --category requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--bench") == 0)
        {
            if (i + 1 < argc)
            {
                bench_filter = argv[++i];
            }
            else
            {
                std::cerr << "Error: --bench requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--duration") == 0)
        {
            if (i + 1 < argc)
            {
                duration_s = std::atof(argv[++i]);
                if (duration_s <= 0.0)
                {
                    std::cerr << "Error: --duration must be positive\n";
                    return 1;
                }
            }
            else
            {
                std::cerr << "Error: --duration requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--output") == 0)
        {
            if (i + 1 < argc)
            {
                output_file = argv[++i];
            }
            else
            {
                std::cerr << "Error: --output requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--enable-microbenchmarks") == 0)
        {
            enable_microbenchmark = true;
        }
        else if (std::strcmp(argv[i], "--list") == 0)
        {
            list_mode = true;
        }
        else if (
            std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    bool want_corosio = std::strcmp(library, "corosio") == 0 ||
        std::strcmp(library, "all") == 0;
    bool want_asio =
        std::strcmp(library, "asio") == 0 || std::strcmp(library, "all") == 0;
    bool want_asio_callback = std::strcmp(library, "asio_callback") == 0 ||
        std::strcmp(library, "all") == 0;

    if (!want_corosio && !want_asio && !want_asio_callback)
    {
        std::cerr << "Error: Unknown library '" << library
                  << "'. Use corosio, asio, asio_callback, or all.\n";
        return 1;
    }

#ifndef BOOST_COROSIO_BENCH_HAS_ASIO
    if (want_asio || want_asio_callback)
    {
        std::cerr << "Error: Boost.Asio benchmarks are not available "
                     "(Boost.Asio was not found at build time).\n";
        return 1;
    }
#endif

    if (!backend)
        backend = perf::default_backend_name();

    return perf::dispatch_backend(
        backend,
        [=]<class BackendTag>(
            perf::context_factory, BackendTag, char const* name) {
            bench::benchmark_runner runner(name, duration_s);

            if (want_corosio)
                add_corosio_suites(runner, BackendTag{});

#ifdef BOOST_COROSIO_BENCH_HAS_ASIO
            if (want_asio)
                add_asio_suites(runner);
            if (want_asio_callback)
                add_asio_callback_suites(runner);
#endif

            if (list_mode)
            {
                runner.list_benchmarks();
                return 0;
            }

            if (want_corosio)
            {
                std::cout << "Boost.Corosio Benchmarks\n";
                std::cout << "========================\n";
                std::cout << "Backend: " << name << "\n";
                std::cout << "Duration: " << duration_s
                          << "s per benchmark\n";
            }

            runner.run(category_filter, bench_filter, enable_microbenchmark);

            std::cout << "\nBenchmarks complete.\n";

            if (output_file)
            {
                if (runner.results().write_json(output_file))
                    std::cout << "Results written to: " << output_file << "\n";
                else
                    std::cerr
                        << "Error: Failed to write results to: " << output_file
                        << "\n";
            }

            return 0;
        });
}
