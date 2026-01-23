//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_BENCH_BENCHMARK_HPP
#define BOOST_COROSIO_BENCH_BENCHMARK_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace bench {

// RAII timer using steady_clock
class stopwatch
{
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;
    using duration = clock::duration;

    stopwatch()
        : start_(clock::now())
    {
    }

    void reset()
    {
        start_ = clock::now();
    }

    duration elapsed() const
    {
        return clock::now() - start_;
    }

    double elapsed_seconds() const
    {
        return std::chrono::duration<double>(elapsed()).count();
    }

    double elapsed_ms() const
    {
        return std::chrono::duration<double, std::milli>(elapsed()).count();
    }

    double elapsed_us() const
    {
        return std::chrono::duration<double, std::micro>(elapsed()).count();
    }

private:
    time_point start_;
};

// Statistics collector
class statistics
{
public:
    void add(double value)
    {
        samples_.push_back(value);
    }

    void clear()
    {
        samples_.clear();
    }

    std::size_t count() const
    {
        return samples_.size();
    }

    double sum() const
    {
        return std::accumulate(samples_.begin(), samples_.end(), 0.0);
    }

    double mean() const
    {
        if (samples_.empty())
            return 0.0;
        return sum() / static_cast<double>(samples_.size());
    }

    double variance() const
    {
        if (samples_.size() < 2)
            return 0.0;
        double m = mean();
        double sq_sum = 0.0;
        for (double v : samples_)
            sq_sum += (v - m) * (v - m);
        return sq_sum / static_cast<double>(samples_.size() - 1);
    }

    double stddev() const
    {
        return std::sqrt(variance());
    }

    double min() const
    {
        if (samples_.empty())
            return 0.0;
        return *std::min_element(samples_.begin(), samples_.end());
    }

    double max() const
    {
        if (samples_.empty())
            return 0.0;
        return *std::max_element(samples_.begin(), samples_.end());
    }

    // Returns the p-th percentile (p in [0, 1])
    double percentile(double p) const
    {
        if (samples_.empty())
            return 0.0;

        std::vector<double> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());

        double index = p * static_cast<double>(sorted.size() - 1);
        std::size_t lower = static_cast<std::size_t>(std::floor(index));
        std::size_t upper = static_cast<std::size_t>(std::ceil(index));

        if (lower == upper)
            return sorted[lower];

        double frac = index - static_cast<double>(lower);
        return sorted[lower] * (1.0 - frac) + sorted[upper] * frac;
    }

    double p50() const { return percentile(0.50); }
    double p90() const { return percentile(0.90); }
    double p99() const { return percentile(0.99); }
    double p999() const { return percentile(0.999); }

private:
    std::vector<double> samples_;
};

// Format operations per second
inline std::string format_rate(double ops_per_sec)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    if (ops_per_sec >= 1e9)
        oss << (ops_per_sec / 1e9) << " Gops/s";
    else if (ops_per_sec >= 1e6)
        oss << (ops_per_sec / 1e6) << " Mops/s";
    else if (ops_per_sec >= 1e3)
        oss << (ops_per_sec / 1e3) << " Kops/s";
    else
        oss << ops_per_sec << " ops/s";

    return oss.str();
}

// Format bytes per second
inline std::string format_throughput(double bytes_per_sec)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    if (bytes_per_sec >= 1e9)
        oss << (bytes_per_sec / 1e9) << " GB/s";
    else if (bytes_per_sec >= 1e6)
        oss << (bytes_per_sec / 1e6) << " MB/s";
    else if (bytes_per_sec >= 1e3)
        oss << (bytes_per_sec / 1e3) << " KB/s";
    else
        oss << bytes_per_sec << " B/s";

    return oss.str();
}

// Format latency in appropriate units
inline std::string format_latency(double microseconds)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    if (microseconds >= 1e6)
        oss << (microseconds / 1e6) << " s";
    else if (microseconds >= 1e3)
        oss << (microseconds / 1e3) << " ms";
    else if (microseconds >= 1.0)
        oss << microseconds << " us";
    else
        oss << (microseconds * 1e3) << " ns";

    return oss.str();
}

// Print a benchmark result header
inline void print_header(char const* name)
{
    std::cout << "\n=== " << name << " ===\n";
}

// Print a benchmark result
inline void print_result(char const* label, double value, char const* unit)
{
    std::cout << "  " << std::left << std::setw(30) << label
              << std::right << std::setw(15) << std::fixed << std::setprecision(2)
              << value << " " << unit << "\n";
}

// Print latency statistics
inline void print_latency_stats(statistics const& stats, char const* label)
{
    std::cout << "  " << label << ":\n";
    std::cout << "    mean:  " << format_latency(stats.mean()) << "\n";
    std::cout << "    p50:   " << format_latency(stats.p50()) << "\n";
    std::cout << "    p90:   " << format_latency(stats.p90()) << "\n";
    std::cout << "    p99:   " << format_latency(stats.p99()) << "\n";
    std::cout << "    p99.9: " << format_latency(stats.p999()) << "\n";
    std::cout << "    min:   " << format_latency(stats.min()) << "\n";
    std::cout << "    max:   " << format_latency(stats.max()) << "\n";
}

} // namespace bench

#endif
