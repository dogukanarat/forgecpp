#pragma once
// Metrics: counters, gauges and histograms, cheap enough for hot paths.
//
//   auto& frames = app.metrics().counter("rx.frames", "RX buffers read");
//   auto& read   = app.metrics().histogram("rx.read_seconds", forge::metrics::latency_buckets(),
//                                           "Time blocked in read_rx");
//   frames.inc();
//   { forge::metrics::Timer t(read); backend.read(...); }
//   app.metrics().gauge_fn("rx.queue", [this] { return double(queue_.size()); });
//
// Exposed on GET /metrics (Prometheus text format, names with '.' -> '_'),
// /forge/api/metrics (JSON), forge.v1 GetMetrics (forgectl metrics) and the
// "metrics" event topic. Instruments are created once and never removed, so
// keep the returned references; updates are lock-free.

#include "forge/v1/observability.pb.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace forge::metrics {

class Counter {
public:
    void inc(std::uint64_t n = 1) { value_.fetch_add(n, std::memory_order_relaxed); }
    std::uint64_t value() const { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> value_{0};
};

class Gauge {
public:
    void set(double v) { value_.store(v, std::memory_order_relaxed); }
    void add(double d);
    double value() const { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<double> value_{0};
};

class Histogram {
public:
    explicit Histogram(std::vector<double> bounds);
    void observe(double v);
    const std::vector<double>& bounds() const { return bounds_; }
    /// Cumulative counts per bound plus +Inf, observation count and sum.
    void snapshot(std::vector<std::uint64_t>& cumulative, std::uint64_t& count, double& sum) const;

private:
    std::vector<double> bounds_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> buckets_;  // per bound + overflow
    std::atomic<std::uint64_t> count_{0};
    std::atomic<double> sum_{0};
};

/// 50 us .. 10 s, for durations in seconds.
std::vector<double> latency_buckets();
/// powers of two from 1 to 2^n.
std::vector<double> exponential_buckets(double start, double factor, int count);

/// Records the scope's duration (seconds) into a histogram.
class Timer {
public:
    explicit Timer(Histogram& h) : h_(h), start_(std::chrono::steady_clock::now()) {}
    ~Timer() { h_.observe(std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count()); }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

private:
    Histogram& h_;
    std::chrono::steady_clock::time_point start_;
};

class Registry {
public:
    Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    /// Get or create. Asking for an existing name with another type throws.
    Counter& counter(const std::string& name, const std::string& help = {}, const std::string& unit = {});
    Gauge& gauge(const std::string& name, const std::string& help = {}, const std::string& unit = {});
    Histogram& histogram(const std::string& name, std::vector<double> bounds, const std::string& help = {},
                         const std::string& unit = {});
    /// Gauge computed when read (e.g. queue sizes). Replaces an earlier one of that name.
    void gauge_fn(const std::string& name, std::function<double()> fn, const std::string& help = {},
                  const std::string& unit = {});

    /// Labels added to every exported metric (e.g. app="radio-controller").
    void set_labels(std::map<std::string, std::string> labels);

    v1::MetricsSnapshot snapshot(const std::string& prefix = {}) const;
    /// Prometheus text exposition format 0.0.4.
    std::string prometheus() const;
    std::vector<std::string> names() const;

private:
    struct Entry {
        v1::Metric::Type type = v1::Metric::TYPE_UNSPECIFIED;
        std::string help, unit;
        std::unique_ptr<Counter> counter;
        std::unique_ptr<Gauge> gauge;
        std::unique_ptr<Histogram> histogram;
        std::function<double()> fn;
    };
    Entry& entry(const std::string& name, v1::Metric::Type type, const std::string& help, const std::string& unit);

    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    std::map<std::string, std::string> labels_;
};

/// Process-wide registry (Application::metrics() returns it).
Registry& registry();

}  // namespace forge::metrics
