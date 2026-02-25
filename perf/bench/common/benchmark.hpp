//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_BENCH_RESULT_HPP
#define BOOST_COROSIO_BENCH_RESULT_HPP

#include "../../common/perf.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace bench {

/** A single metric with a name and numeric value. */
struct metric
{
    std::string name;
    double value;

    metric(std::string n, double v) : name(std::move(n)), value(v) {}
};

/** Result from a single benchmark run. */
struct benchmark_result
{
    std::string library;
    std::string category;
    std::string name;
    std::vector<metric> metrics;

    benchmark_result(
        std::string lib,
        std::string cat,
        std::string n)
        : library(std::move(lib))
        , category(std::move(cat))
        , name(std::move(n))
    {
    }

    benchmark_result& add(std::string metric_name, double value)
    {
        metrics.emplace_back(std::move(metric_name), value);
        return *this;
    }

    /** Add all statistics from a statistics object with a prefix. */
    benchmark_result&
    add_latency_stats(std::string prefix, perf::statistics const& stats)
    {
        add(prefix + "_mean_ns", stats.mean());
        add(prefix + "_p50_ns", stats.p50());
        add(prefix + "_p90_ns", stats.p90());
        add(prefix + "_p99_ns", stats.p99());
        add(prefix + "_p999_ns", stats.p999());
        add(prefix + "_min_ns", (stats.min)());
        add(prefix + "_max_ns", (stats.max)());
        return *this;
    }
};

/** Collect benchmark results and serialize to JSON. */
class result_collector
{
    std::string backend_;
    std::string timestamp_;
    double duration_s_ = 0.0;
    std::vector<benchmark_result> results_;

    static std::string escape_json(std::string const& s)
    {
        std::ostringstream oss;
        for (char c : s)
        {
            switch (c)
            {
            case '"':
                oss << "\\\"";
                break;
            case '\\':
                oss << "\\\\";
                break;
            case '\b':
                oss << "\\b";
                break;
            case '\f':
                oss << "\\f";
                break;
            case '\n':
                oss << "\\n";
                break;
            case '\r':
                oss << "\\r";
                break;
            case '\t':
                oss << "\\t";
                break;
            default:
                oss << c;
                break;
            }
        }
        return oss.str();
    }

    static std::string current_timestamp()
    {
        auto now  = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
#ifdef _WIN32
        localtime_s(&tm_buf, &time);
#else
        localtime_r(&time, &tm_buf);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
        return oss.str();
    }

public:
    explicit result_collector(std::string backend = "")
        : backend_(std::move(backend))
        , timestamp_(current_timestamp())
    {
    }

    void set_backend(std::string backend)
    {
        backend_ = std::move(backend);
    }
    void set_duration(double duration_s)
    {
        duration_s_ = duration_s;
    }

    void add(benchmark_result result)
    {
        results_.push_back(std::move(result));
    }

    /** Serialize all results to JSON. */
    std::string to_json() const
    {
        std::ostringstream oss;
        oss << std::setprecision(10);

        oss << "{\n";
        oss << "  \"metadata\": {\n";
        oss << "    \"backend\": \"" << escape_json(backend_) << "\",\n";
        oss << "    \"timestamp\": \"" << escape_json(timestamp_) << "\",\n";
        oss << "    \"duration_s\": " << duration_s_ << "\n";
        oss << "  },\n";
        oss << "  \"benchmarks\": [\n";

        for (std::size_t i = 0; i < results_.size(); ++i)
        {
            auto const& r = results_[i];
            oss << "    {\n";
            if (!r.library.empty())
                oss << "      \"library\": \""
                    << escape_json(r.library) << "\",\n";
            oss << "      \"category\": \""
                << escape_json(r.category) << "\",\n";
            oss << "      \"name\": \"" << escape_json(r.name) << "\"";

            for (auto const& m : r.metrics)
                oss << ",\n      \"" << escape_json(m.name)
                    << "\": " << m.value;

            oss << "\n    }";
            if (i + 1 < results_.size())
                oss << ",";
            oss << "\n";
        }

        oss << "  ]\n";
        oss << "}\n";

        return oss.str();
    }

    /** Write JSON to a file. Returns true on success. */
    bool write_json(std::string const& path) const
    {
        std::ofstream out(path);
        if (!out)
            return false;
        out << to_json();
        return out.good();
    }
};

} // namespace bench

#endif
