//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "benchmarks.hpp"

#include <cstring>
#include <iostream>

#include "../common/benchmark.hpp"

namespace {

void run_benchmarks(
    char const* output_file,
    char const* category_filter,
    char const* bench_filter )
{
    std::cout << "Boost.Asio Benchmarks\n";
    std::cout << "=====================\n";

    bench::result_collector collector( "asio" );

    bool run_all = !category_filter || std::strcmp( category_filter, "all" ) == 0;

    if( run_all || std::strcmp( category_filter, "io_context" ) == 0 )
        asio_bench::run_io_context_benchmarks( collector, bench_filter );

    if( run_all || std::strcmp( category_filter, "socket_throughput" ) == 0 )
        asio_bench::run_socket_throughput_benchmarks( collector, bench_filter );

    if( run_all || std::strcmp( category_filter, "socket_latency" ) == 0 )
        asio_bench::run_socket_latency_benchmarks( collector, bench_filter );

    if( run_all || std::strcmp( category_filter, "http_server" ) == 0 )
        asio_bench::run_http_server_benchmarks( collector, bench_filter );

    std::cout << "\nBenchmarks complete.\n";

    if( output_file )
    {
        if( collector.write_json( output_file ) )
            std::cout << "Results written to: " << output_file << "\n";
        else
            std::cerr << "Error: Failed to write results to: " << output_file << "\n";
    }
}

void print_usage( char const* program_name )
{
    std::cout << "Usage: " << program_name << " [OPTIONS]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --category <name>   Run only the specified benchmark category\n";
    std::cout << "  --bench <name>      Run only the specified benchmark within category\n";
    std::cout << "  --output <file>     Write JSON results to file\n";
    std::cout << "  --help              Show this help message\n";
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
}

} // anonymous namespace

int main( int argc, char* argv[] )
{
    char const* output_file = nullptr;
    char const* category_filter = nullptr;
    char const* bench_filter = nullptr;

    for( int i = 1; i < argc; ++i )
    {
        if( std::strcmp( argv[i], "--category" ) == 0 )
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

    run_benchmarks( output_file, category_filter, bench_filter );
    return 0;
}
