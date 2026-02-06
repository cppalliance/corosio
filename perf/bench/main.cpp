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
#include "asio/benchmarks.hpp"
#endif

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/detail/platform.hpp>

#include <cstdlib>
#include <cstring>
#include <iostream>

#include "../common/backend_selection.hpp"
#include "common/benchmark.hpp"

namespace {

void print_usage( char const* program_name )
{
    std::cout << "Usage: " << program_name << " [OPTIONS]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --library <name>    Library to benchmark (default: corosio)\n";
    std::cout << "  --backend <name>    Select I/O backend (default: platform default)\n";
    std::cout << "  --category <name>   Run only the specified benchmark category\n";
    std::cout << "  --bench <name>      Run only the specified benchmark within category\n";
    std::cout << "  --duration <secs>   Duration per benchmark in seconds (default: 3.0)\n";
    std::cout << "  --output <file>     Write JSON results to file\n";
    std::cout << "  --list              List available backends\n";
    std::cout << "  --help              Show this help message\n";
    std::cout << "\n";
    std::cout << "Libraries (--library):\n";
    std::cout << "  corosio             Boost.Corosio benchmarks (default)\n";
#ifdef BOOST_COROSIO_BENCH_HAS_ASIO
    std::cout << "  asio                Boost.Asio comparison benchmarks\n";
    std::cout << "  all                 Run both libraries\n";
#else
    std::cout << "  asio                (not available — Boost.Asio not found)\n";
    std::cout << "  all                 (not available — Boost.Asio not found)\n";
#endif
    std::cout << "\n";
    std::cout << "Benchmark categories:\n";
    std::cout << "  io_context          io_context handler throughput tests\n";
    std::cout << "  socket_throughput   Socket throughput tests\n";
    std::cout << "  socket_latency      Socket latency tests\n";
    std::cout << "  http_server         HTTP server benchmarks\n";
    std::cout << "  all                 Run all categories (default)\n";
    std::cout << "\n";
    std::cout << "Individual benchmarks (--bench):\n";
    std::cout << "  io_context:         single_threaded, multithreaded, interleaved, concurrent\n";
    std::cout << "  socket_throughput:  unidirectional, bidirectional\n";
    std::cout << "  socket_latency:     pingpong, concurrent\n";
    std::cout << "  http_server:        single_conn, concurrent, multithread\n";
    std::cout << "\n";
    perf::print_available_backends();
}

} // anonymous namespace

int main( int argc, char* argv[] )
{
    char const* library = "corosio";
    char const* backend = nullptr;
    char const* output_file = nullptr;
    char const* category_filter = nullptr;
    char const* bench_filter = nullptr;
    double duration_s = 3.0;

    for( int i = 1; i < argc; ++i )
    {
        if( std::strcmp( argv[i], "--library" ) == 0 )
        {
            if( i + 1 < argc )
            {
                library = argv[++i];
            }
            else
            {
                std::cerr << "Error: --library requires an argument\n";
                return 1;
            }
        }
        else if( std::strcmp( argv[i], "--backend" ) == 0 )
        {
            if( i + 1 < argc )
            {
                backend = argv[++i];
            }
            else
            {
                std::cerr << "Error: --backend requires an argument\n";
                return 1;
            }
        }
        else if( std::strcmp( argv[i], "--category" ) == 0 )
        {
            if( i + 1 < argc )
            {
                category_filter = argv[++i];
            }
            else
            {
                std::cerr << "Error: --category requires an argument\n";
                return 1;
            }
        }
        else if( std::strcmp( argv[i], "--bench" ) == 0 )
        {
            if( i + 1 < argc )
            {
                bench_filter = argv[++i];
            }
            else
            {
                std::cerr << "Error: --bench requires an argument\n";
                return 1;
            }
        }
        else if( std::strcmp( argv[i], "--duration" ) == 0 )
        {
            if( i + 1 < argc )
            {
                duration_s = std::atof( argv[++i] );
                if( duration_s <= 0.0 )
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
        else if( std::strcmp( argv[i], "--output" ) == 0 )
        {
            if( i + 1 < argc )
            {
                output_file = argv[++i];
            }
            else
            {
                std::cerr << "Error: --output requires an argument\n";
                return 1;
            }
        }
        else if( std::strcmp( argv[i], "--list" ) == 0 )
        {
            perf::print_available_backends();
            return 0;
        }
        else if( std::strcmp( argv[i], "--help" ) == 0 || std::strcmp( argv[i], "-h" ) == 0 )
        {
            print_usage( argv[0] );
            return 0;
        }
        else
        {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage( argv[0] );
            return 1;
        }
    }

    bool want_corosio = std::strcmp( library, "corosio" ) == 0 || std::strcmp( library, "all" ) == 0;
    bool want_asio = std::strcmp( library, "asio" ) == 0 || std::strcmp( library, "all" ) == 0;

    if( !want_corosio && !want_asio )
    {
        std::cerr << "Error: Unknown library '" << library << "'. Use corosio, asio, or all.\n";
        return 1;
    }

#ifndef BOOST_COROSIO_BENCH_HAS_ASIO
    if( want_asio )
    {
        std::cerr << "Error: Boost.Asio benchmarks are not available (Boost.Asio was not found at build time).\n";
        return 1;
    }
#endif

    if( !backend )
        backend = perf::default_backend_name();

    return perf::dispatch_backend( backend,
        [=]( perf::context_factory factory, char const* name )
        {
            bench::result_collector collector( name );
            collector.set_duration( duration_s );

            if( !want_corosio )
                collector.set_backend( "asio" );

            if( want_corosio )
            {
                std::cout << "Boost.Corosio Benchmarks\n";
                std::cout << "========================\n";
                std::cout << "Backend: " << name << "\n";
                std::cout << "Duration: " << duration_s << " s per benchmark\n";
            }

            bool run_all_cats = !category_filter || std::strcmp( category_filter, "all" ) == 0;

            // Whether bench_filter allows a given benchmark name
            auto want_bench = [&]( char const* b )
            {
                return !bench_filter
                    || std::strcmp( bench_filter, "all" ) == 0
                    || std::strcmp( bench_filter, b ) == 0;
            };

            if( run_all_cats || std::strcmp( category_filter, "io_context" ) == 0 )
            {
                char const* benches[] = { "single_threaded", "multithreaded", "interleaved", "concurrent" };
                for( auto* b : benches )
                {
                    if( !want_bench( b ) )
                        continue;
                    if( want_corosio )
                        corosio_bench::run_io_context_benchmarks( factory, collector, b, duration_s );
#ifdef BOOST_COROSIO_BENCH_HAS_ASIO
                    if( want_asio )
                        asio_bench::run_io_context_benchmarks( collector, b, duration_s );
#endif
                }
            }

            if( run_all_cats || std::strcmp( category_filter, "socket_throughput" ) == 0 )
            {
                char const* benches[] = { "unidirectional", "bidirectional" };
                for( auto* b : benches )
                {
                    if( !want_bench( b ) )
                        continue;
                    if( want_corosio )
                        corosio_bench::run_socket_throughput_benchmarks( factory, collector, b, duration_s );
#ifdef BOOST_COROSIO_BENCH_HAS_ASIO
                    if( want_asio )
                        asio_bench::run_socket_throughput_benchmarks( collector, b, duration_s );
#endif
                }
            }

            if( run_all_cats || std::strcmp( category_filter, "socket_latency" ) == 0 )
            {
                char const* benches[] = { "pingpong", "concurrent" };
                for( auto* b : benches )
                {
                    if( !want_bench( b ) )
                        continue;
                    if( want_corosio )
                        corosio_bench::run_socket_latency_benchmarks( factory, collector, b, duration_s );
#ifdef BOOST_COROSIO_BENCH_HAS_ASIO
                    if( want_asio )
                        asio_bench::run_socket_latency_benchmarks( collector, b, duration_s );
#endif
                }
            }

            if( run_all_cats || std::strcmp( category_filter, "http_server" ) == 0 )
            {
                char const* benches[] = { "single_conn", "concurrent", "multithread" };
                for( auto* b : benches )
                {
                    if( !want_bench( b ) )
                        continue;
                    if( want_corosio )
                        corosio_bench::run_http_server_benchmarks( factory, collector, b, duration_s );
#ifdef BOOST_COROSIO_BENCH_HAS_ASIO
                    if( want_asio )
                        asio_bench::run_http_server_benchmarks( collector, b, duration_s );
#endif
                }
            }

            std::cout << "\nBenchmarks complete.\n";

            if( output_file )
            {
                if( collector.write_json( output_file ) )
                    std::cout << "Results written to: " << output_file << "\n";
                else
                    std::cerr << "Error: Failed to write results to: " << output_file << "\n";
            }
        } );
}
