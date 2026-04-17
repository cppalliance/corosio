//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_BENCH_SUITE_HPP
#define BOOST_COROSIO_BENCH_SUITE_HPP

#include "benchmark.hpp"
#include "../../common/perf.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bench {

/// Flags controlling suite and entry behavior.
enum class bench_flags : unsigned
{
    none                  = 0,
    needs_conntrack_drain = 1u << 0,
    is_microbenchmark     = 1u << 1,
};

inline bench_flags
operator|(bench_flags a, bench_flags b)
{
    return static_cast<bench_flags>(
        static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

inline bench_flags
operator&(bench_flags a, bench_flags b)
{
    return static_cast<bench_flags>(
        static_cast<unsigned>(a) & static_cast<unsigned>(b));
}

inline bool
has_flag(bench_flags flags, bench_flags test)
{
    return (flags & test) != bench_flags::none;
}

/** RAII guard that records one iteration's latency.

    Counts an op and records elapsed time on destruction.
*/
class lap_guard
{
    perf::stopwatch sw_;
    perf::statistics& stats_;
    std::atomic<int64_t>& ops_;
    std::mutex& mtx_;

public:
    lap_guard(
        perf::statistics& stats,
        std::atomic<int64_t>& ops,
        std::mutex& mtx)
        : stats_(stats)
        , ops_(ops)
        , mtx_(mtx)
    {
    }

    ~lap_guard()
    {
        double ns = sw_.elapsed_ns();
        ops_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lk(mtx_);
        stats_.add(ns);
    }

    lap_guard(lap_guard const&)            = delete;
    lap_guard& operator=(lap_guard const&) = delete;
};

/** Per-benchmark state passed to every benchmark function.

    The runner creates a `state`, calls the benchmark function, then
    extracts timing, counters, and latency data from the state to
    build standardized output and JSON results.
*/
class state
{
    double duration_s_;
    std::vector<int64_t> ranges_;

    std::atomic<bool> running_{true};
    std::atomic<int64_t> ops_{0};
    std::atomic<int64_t> bytes_{0};
    int64_t items_  = 0;
    double elapsed_ = 0.0;
    perf::statistics latency_stats_;
    std::mutex latency_mutex_;

public:
    /// Custom counters (like Google Benchmark's state.counters).
    std::unordered_map<std::string, double> counters;

    explicit state(double duration_s, std::vector<int64_t> ranges = {})
        : duration_s_(duration_s)
        , ranges_(std::move(ranges))
    {
    }

    /// Return true while the benchmark should keep iterating.
    bool running() const
    {
        return running_.load(std::memory_order_relaxed);
    }

    /// Stop the benchmark loop.
    void stop()
    {
        running_.store(false, std::memory_order_relaxed);
    }

    /** Block the calling thread until the configured duration expires.

        Sets `running()` to false when done. Use for multi-threaded
        benchmarks where the main thread waits while workers iterate.
    */
    void wait()
    {
        perf::stopwatch sw;
        std::this_thread::sleep_for(
            std::chrono::duration<double>(duration_s_));
        running_.store(false, std::memory_order_relaxed);
        elapsed_ = sw.elapsed_seconds();
    }

    /** Start a timer thread and record elapsed time on completion.

        Call this after spawning coroutines but before `ioc.run()`.
        The timer thread sleeps for `duration_s` then sets
        `running()` to false.

        @return A stopwatch that the caller should query after
            `ioc.run()` returns, then pass to `set_elapsed()`.
    */
    perf::stopwatch start_timer_thread()
    {
        perf::stopwatch sw;
        std::thread([this] {
            std::this_thread::sleep_for(
                std::chrono::duration<double>(duration_s_));
            running_.store(false, std::memory_order_relaxed);
        }).detach();
        return sw;
    }

    /// Record the elapsed time from an external stopwatch.
    void set_elapsed(double seconds)
    {
        elapsed_ = seconds;
    }

    /** Create an RAII lap guard.

        Counts one op and records its latency on destruction.
    */
    [[nodiscard]] lap_guard lap()
    {
        return lap_guard(latency_stats_, ops_, latency_mutex_);
    }

    /// Return the atomic ops counter.
    std::atomic<int64_t>& ops()
    {
        return ops_;
    }

    /// Accumulate bytes for throughput reporting (thread-safe).
    void add_bytes(int64_t n)
    {
        bytes_.fetch_add(n, std::memory_order_relaxed);
    }

    /// Accumulate items for rate reporting.
    void add_items(int64_t n)
    {
        items_ += n;
    }

    /// Return the i-th range parameter.
    int64_t range(int idx) const
    {
        return ranges_.at(idx);
    }

    /// Return the number of range parameters.
    int range_count() const
    {
        return static_cast<int>(ranges_.size());
    }

    /// Return the configured duration in seconds.
    double duration() const
    {
        return duration_s_;
    }

    /// Return elapsed time after wait() or set_elapsed().
    double elapsed_seconds() const
    {
        return elapsed_;
    }

    /// Record a latency sample in nanoseconds (thread-safe).
    void record_latency(double ns)
    {
        std::lock_guard lk(latency_mutex_);
        latency_stats_.add(ns);
    }

    /// Return the latency statistics collector.
    perf::statistics& latency()
    {
        return latency_stats_;
    }

    /// Return the latency statistics collector (const).
    perf::statistics const& latency() const
    {
        return latency_stats_;
    }

    /// Return total ops counted.
    int64_t total_ops() const
    {
        return ops_.load(std::memory_order_relaxed);
    }

    /// Return total bytes accumulated.
    int64_t total_bytes() const
    {
        return bytes_.load(std::memory_order_relaxed);
    }

    /// Return total items accumulated.
    int64_t total_items() const
    {
        return items_;
    }
};

/** A single benchmark entry within a suite. */
struct suite_entry
{
    std::string name;
    std::function<void(state&)> fn;
    bench_flags flags = bench_flags::none;
    std::vector<int64_t> args;
};

/** Group of related benchmarks sharing a category name. */
class benchmark_suite
{
    std::string library_;
    std::string category_;
    bench_flags flags_;
    std::vector<suite_entry> entries_;

public:
    using bench_fn = std::function<void(state&)>;

    explicit benchmark_suite(
        std::string category, bench_flags flags = bench_flags::none)
        : category_(std::move(category))
        , flags_(flags)
    {
    }

    /// Add a benchmark with no parameters.
    benchmark_suite&
    add(std::string name, bench_fn fn,
        bench_flags flags = bench_flags::none)
    {
        entries_.push_back({std::move(name), std::move(fn), flags, {}});
        return *this;
    }

    /// Set argument values for the most recently added benchmark.
    benchmark_suite& args(std::vector<int64_t> values)
    {
        if (!entries_.empty())
            entries_.back().args = std::move(values);
        return *this;
    }

    /// Generate a range of argument values for the most recently
    /// added benchmark: lo, lo*mul, lo*mul*mul, ... up to hi.
    benchmark_suite& range(int64_t lo, int64_t hi, int64_t mul)
    {
        std::vector<int64_t> values;
        for (int64_t v = lo; v <= hi; v *= mul)
            values.push_back(v);
        if (!entries_.empty())
            entries_.back().args = std::move(values);
        return *this;
    }

    /// Set the library name (called by the runner).
    void set_library(std::string lib) { library_ = std::move(lib); }

    std::string const& library() const { return library_; }
    std::string const& category() const { return category_; }
    bench_flags flags() const { return flags_; }
    std::vector<suite_entry> const& entries() const { return entries_; }
};

/** Orchestrate benchmark execution, output, and result collection. */
class benchmark_runner
{
    std::string backend_;
    double duration_s_;
    double warmup_duration_s_ = 0.0;
    std::vector<benchmark_suite> suites_;
    result_collector collector_;

public:
    benchmark_runner(std::string backend_name, double duration_s)
        : backend_(std::move(backend_name))
        , duration_s_(duration_s)
        , collector_(backend_)
    {
        collector_.set_duration(duration_s);
    }

    /** Set the per-benchmark warmup duration in seconds.

        When greater than zero, every benchmark runs once with a throwaway
        `state` for this duration before the real measurement. The warmup
        exercises the exact code path being measured, priming kernel
        buffers, allocator pools, CPU caches, and library-specific
        dispatch paths. Results from the warmup run are discarded.

        @par Rationale
        A bench-specific lambda warmup primes the wrong paths if the
        lambda uses a different I/O style than the benchmark (e.g. sync
        I/O vs async). Self-warmup guarantees the primed code path is
        the measured code path.
    */
    void set_warmup_duration(double seconds)
    {
        warmup_duration_s_ = seconds;
    }

    double warmup_duration() const { return warmup_duration_s_; }

    /// Add a suite to the runner.
    void add_suite(benchmark_suite suite)
    {
        suites_.push_back(std::move(suite));
    }

    /// Add a suite tagged with a library name.
    void add_suite(std::string library, benchmark_suite suite)
    {
        suite.set_library(std::move(library));
        suites_.push_back(std::move(suite));
    }

    /// List all benchmarks without running them.
    void list_benchmarks() const
    {
        for (auto const& suite : suites_)
        {
            if (!suite.library().empty())
                std::cout << suite.library() << ":";
            std::cout << suite.category() << ":\n";
            for (auto const& entry : suite.entries())
            {
                if (entry.args.empty())
                {
                    std::cout << "  " << entry.name << "\n";
                }
                else
                {
                    for (auto v : entry.args)
                        std::cout << "  " << entry.name
                                  << "/" << v << "\n";
                }
            }
        }
    }

    /** Run benchmarks matching the given filters.

        @param category_filter nullptr or "all" runs all categories,
            otherwise exact-match on category name.
        @param bench_filter nullptr or "all" runs all benchmarks,
            otherwise prefix-match on entry name.
        @param enable_microbenchmarks If false, suites flagged
            `is_microbenchmark` are skipped unless explicitly
            selected by category_filter.
    */
    void run(
        char const* category_filter,
        char const* bench_filter,
        bool enable_microbenchmarks)
    {
        bool run_all_cats = !category_filter ||
            std::strcmp(category_filter, "all") == 0;

        auto want_bench = [&](std::string const& name) {
            if (!bench_filter || std::strcmp(bench_filter, "all") == 0)
                return true;
            // Prefix match
            return name.compare(
                0, std::strlen(bench_filter), bench_filter) == 0;
        };

        for (auto const& suite : suites_)
        {
            bool explicit_cat = category_filter &&
                std::strcmp(category_filter, suite.category().c_str()) == 0;

            if (!run_all_cats && !explicit_cat)
                continue;

            // Skip microbenchmarks unless explicitly requested
            if (has_flag(suite.flags(), bench_flags::is_microbenchmark) &&
                !explicit_cat && !enable_microbenchmarks)
                continue;

            for (auto const& entry : suite.entries())
            {
                bool needs_drain =
                    has_flag(suite.flags(),
                        bench_flags::needs_conntrack_drain) ||
                    has_flag(entry.flags,
                        bench_flags::needs_conntrack_drain);

                if (entry.args.empty())
                {
                    if (!want_bench(entry.name))
                        continue;

                    run_entry(
                        suite.library(), suite.category(),
                        entry.name, entry.fn, {}, needs_drain);
                }
                else
                {
                    for (auto v : entry.args)
                    {
                        std::string full_name =
                            entry.name + "/" + std::to_string(v);
                        if (!want_bench(entry.name) &&
                            !want_bench(full_name))
                            continue;

                        run_entry(
                            suite.library(), suite.category(),
                            full_name, entry.fn, {v}, needs_drain);
                    }
                }
            }
        }
    }

    /// Return the result collector.
    result_collector const& results() const
    {
        return collector_;
    }

private:
    void run_entry(
        std::string const& library,
        std::string const& category,
        std::string const& name,
        std::function<void(state&)> const& fn,
        std::vector<int64_t> ranges,
        bool needs_drain)
    {
        auto maybe_drain = [&] {
            if (needs_drain)
                perf::await_conntrack_drain();
        };

        // Self-warmup: run the benchmark with a throwaway state so the
        // exact code path being measured is primed (kernel buffers,
        // allocator pools, CPU caches, library dispatch). Drain again
        // between warmup and real run to clear any socket state the
        // warmup left behind.
        if (warmup_duration_s_ > 0.0)
        {
            maybe_drain();
            state warmup_st(warmup_duration_s_, ranges);
            fn(warmup_st);
        }

        maybe_drain();

        std::string header;
        if (!library.empty())
            header += "(" + library + ") ";
        header += "[" + category + "] " + name;
        perf::print_header(header.c_str());

        state st(duration_s_, std::move(ranges));
        fn(st);

        print_results(st);
        collect_results(library, category, name, st);
    }

    void print_results(state const& st)
    {
        double elapsed = st.elapsed_seconds();
        if (elapsed <= 0.0)
            return;

        int64_t ops = st.total_ops();
        if (ops > 0)
        {
            double ops_per_sec = static_cast<double>(ops) / elapsed;
            std::cout << "  Ops:           " << ops << "\n";
            std::cout << "  Throughput:    "
                      << perf::format_rate(ops_per_sec) << "\n";
        }

        int64_t items = st.total_items();
        if (items > 0)
        {
            double items_per_sec = static_cast<double>(items) / elapsed;
            std::cout << "  Items:         " << items << "\n";
            std::cout << "  Rate:          "
                      << perf::format_rate(items_per_sec) << "\n";
        }

        int64_t bytes = st.total_bytes();
        if (bytes > 0)
        {
            double bytes_per_sec = static_cast<double>(bytes) / elapsed;
            std::cout << "  Bytes:         " << bytes << "\n";
            std::cout << "  Throughput:    "
                      << perf::format_throughput(bytes_per_sec) << "\n";
        }

        std::cout << "  Elapsed:       " << std::fixed
                  << std::setprecision(3) << elapsed << " s\n";

        if (st.latency().count() > 0)
            perf::print_latency_stats(st.latency(), "Latency");

        for (auto const& [k, v] : st.counters)
        {
            std::string label;
            bool cap = true;
            for (char c : k)
            {
                if (c == '_')
                {
                    label += ' ';
                    cap = true;
                }
                else if (cap)
                {
                    if (c >= 'a' && c <= 'z')
                        label += static_cast<char>(c - 'a' + 'A');
                    else
                        label += c;
                    cap = false;
                }
                else
                {
                    label += c;
                }
            }
            label += ':';
            std::cout << "  " << std::left << std::setw(15)
                      << label;
            if (v == static_cast<int64_t>(v))
                std::cout << static_cast<int64_t>(v);
            else
                std::cout << v;
            std::cout << "\n";
        }

        std::cout << "\n";
    }

    void collect_results(
        std::string const& library,
        std::string const& category,
        std::string const& name,
        state const& st)
    {
        double elapsed = st.elapsed_seconds();
        if (elapsed <= 0.0)
            return;

        benchmark_result result(library, category, name);
        result.add("elapsed_s", elapsed);

        int64_t ops = st.total_ops();
        if (ops > 0)
        {
            result.add("ops", static_cast<double>(ops));
            result.add("ops_per_sec",
                static_cast<double>(ops) / elapsed);
        }

        int64_t items = st.total_items();
        if (items > 0)
        {
            result.add("items", static_cast<double>(items));
            result.add("items_per_sec",
                static_cast<double>(items) / elapsed);
        }

        int64_t bytes = st.total_bytes();
        if (bytes > 0)
        {
            result.add("bytes", static_cast<double>(bytes));
            result.add("bytes_per_sec",
                static_cast<double>(bytes) / elapsed);
        }

        if (st.latency().count() > 0)
            result.add_latency_stats("latency", st.latency());

        for (auto const& [k, v] : st.counters)
            result.add(k, v);

        collector_.add(std::move(result));
    }
};

} // namespace bench

#endif
