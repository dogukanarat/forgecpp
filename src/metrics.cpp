#include "forge/metrics.hpp"

#include "forge/log.hpp"

#include <format>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace forge::metrics {
namespace {

void atomic_add(std::atomic<double>& a, double d) {
    double cur = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(cur, cur + d, std::memory_order_relaxed)) {
    }
}

std::string prom_name(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':') ? c : '_';
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
    return out;
}

std::string prom_escape(const std::string& v) {
    std::string out;
    for (char c : v) {
        if (c == '\\' || c == '"') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out;
}

std::string prom_number(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
    return std::format("{}", v);
}

}  // namespace

void Gauge::add(double d) { atomic_add(value_, d); }

Histogram::Histogram(std::vector<double> bounds) : bounds_(std::move(bounds)) {
    std::sort(bounds_.begin(), bounds_.end());
    bounds_.erase(std::unique(bounds_.begin(), bounds_.end()), bounds_.end());
    buckets_ = std::make_unique<std::atomic<std::uint64_t>[]>(bounds_.size() + 1);
}

void Histogram::observe(double v) {
    const auto it = std::lower_bound(bounds_.begin(), bounds_.end(), v);
    buckets_[static_cast<std::size_t>(it - bounds_.begin())].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    atomic_add(sum_, v);
}

void Histogram::snapshot(std::vector<std::uint64_t>& cumulative, std::uint64_t& count, double& sum) const {
    cumulative.assign(bounds_.size() + 1, 0);
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i <= bounds_.size(); ++i) {
        acc += buckets_[i].load(std::memory_order_relaxed);
        cumulative[i] = acc;
    }
    count = acc;
    sum = sum_.load(std::memory_order_relaxed);
}

std::vector<double> latency_buckets() {
    return {50e-6, 100e-6, 250e-6, 500e-6, 1e-3, 2.5e-3, 5e-3, 10e-3, 25e-3,
            50e-3, 100e-3, 250e-3, 500e-3, 1,    2.5,    5,    10};
}

std::vector<double> exponential_buckets(double start, double factor, int count) {
    std::vector<double> b;
    double v = start;
    for (int i = 0; i < count; ++i, v *= factor) b.push_back(v);
    return b;
}

Registry::Entry& Registry::entry(const std::string& name, Type type, const std::string& help, const std::string& unit) {
    auto [it, inserted] = entries_.try_emplace(name);
    Entry& e = it->second;
    if (inserted) {
        e.type = type;
        e.help = help;
        e.unit = unit;
    } else if (e.type != type) {
        throw std::invalid_argument("metric '" + name + "' already exists with another type");
    }
    return e;
}

Counter& Registry::counter(const std::string& name, const std::string& help, const std::string& unit) {
    std::lock_guard lock(mutex_);
    auto& e = entry(name, Type::counter, help, unit);
    if (!e.counter) e.counter = std::make_unique<Counter>();
    return *e.counter;
}

Gauge& Registry::gauge(const std::string& name, const std::string& help, const std::string& unit) {
    std::lock_guard lock(mutex_);
    auto& e = entry(name, Type::gauge, help, unit);
    if (!e.gauge && !e.fn) e.gauge = std::make_unique<Gauge>();
    if (!e.gauge) throw std::invalid_argument("metric '" + name + "' is a computed gauge");
    return *e.gauge;
}

Histogram& Registry::histogram(const std::string& name, std::vector<double> bounds, const std::string& help,
                               const std::string& unit) {
    std::lock_guard lock(mutex_);
    auto& e = entry(name, Type::histogram, help, unit);
    if (!e.histogram) e.histogram = std::make_unique<Histogram>(std::move(bounds));
    return *e.histogram;
}

void Registry::gauge_fn(const std::string& name, std::function<double()> fn, const std::string& help,
                        const std::string& unit) {
    std::lock_guard lock(mutex_);
    auto& e = entry(name, Type::gauge, help, unit);
    e.gauge.reset();
    e.fn = std::move(fn);
}

void Registry::set_labels(std::map<std::string, std::string> labels) {
    std::lock_guard lock(mutex_);
    labels_ = std::move(labels);
}

std::vector<std::string> Registry::names() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [n, e] : entries_) out.push_back(n);
    return out;
}

Snapshot Registry::snapshot(const std::string& prefix) const {
    Snapshot s;
    s.time_unix_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    // The callbacks of the gauges run after the lock is released: one that
    // reads another metric would otherwise deadlock, and a slow one would
    // hold every thread that creates a metric.
    struct Deferred {
        std::size_t index;
        std::function<double()> fn;
    };
    std::vector<Deferred> deferred;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [name, e] : entries_) {
            if (!prefix.empty() && name.compare(0, prefix.size(), prefix) != 0) continue;
            Sample m;
            m.name = name;
            m.help = e.help;
            m.unit = e.unit;
            m.type = e.type;
            m.labels = labels_;
            if (e.counter) m.value = static_cast<double>(e.counter->value());
            if (e.gauge) m.value = e.gauge->value();
            if (e.fn) deferred.push_back({s.metrics.size(), e.fn});
            if (e.histogram) {
                e.histogram->snapshot(m.buckets, m.count, m.sum);
                m.bounds = e.histogram->bounds();
            }
            s.metrics.push_back(std::move(m));
        }
    }
    for (const auto& d : deferred) {
        auto& m = s.metrics[d.index];
        bool failed = false;
        try {
            m.value = d.fn();
        } catch (const std::exception& ex) {
            m.value = std::nan("");  // shown as NaN
            failed = true;
            std::lock_guard lock(mutex_);
            auto it = entries_.find(m.name);
            if (it != entries_.end() && !it->second.fn_failed)
                log::warn("metrics: gauge {} failed: {}", m.name, ex.what());
        }
        std::lock_guard lock(mutex_);
        auto it = entries_.find(m.name);
        if (it != entries_.end()) it->second.fn_failed = failed;
    }
    return s;
}

const char* to_string(Type t) {
    switch (t) {
        case Type::counter: return "counter";
        case Type::gauge: return "gauge";
        case Type::histogram: return "histogram";
    }
    return "counter";
}

json to_json(const Snapshot& s) {
    json metrics = json::array();
    for (const auto& m : s.metrics) {
        json j{{"name", m.name}, {"help", m.help}, {"type", to_string(m.type)}, {"unit", m.unit}};
        if (m.type == Type::histogram) {
            j["bounds"] = m.bounds;
            j["buckets"] = m.buckets;
            j["count"] = m.count;
            j["sum"] = m.sum;
        } else {
            j["value"] = std::isfinite(m.value) ? json(m.value) : json(nullptr);
        }
        if (!m.labels.empty()) j["labels"] = m.labels;
        metrics.push_back(std::move(j));
    }
    return {{"time_unix_ns", s.time_unix_ns}, {"metrics", std::move(metrics)}};
}

std::string Registry::prometheus() const {
    const auto snap = snapshot();
    std::string out;
    for (const auto& m : snap.metrics) {
        const std::string n = prom_name(m.name);
        std::string labels;
        for (const auto& [k, v] : m.labels) {
            labels += std::format("{}{}=\"{}\"", labels.empty() ? "" : ",", prom_name(k), prom_escape(v));
        }
        auto with = [&](const std::string& extra) {
            const std::string all = labels.empty() ? extra : (extra.empty() ? labels : labels + "," + extra);
            return all.empty() ? std::string() : "{" + all + "}";
        };
        if (!m.help.empty()) out += std::format("# HELP {} {}\n", n, prom_escape(m.help));
        switch (m.type) {
            case Type::counter:
                out += std::format("# TYPE {} counter\n{}{} {}\n", n, n, with(""), prom_number(m.value));
                break;
            case Type::gauge:
                out += std::format("# TYPE {} gauge\n{}{} {}\n", n, n, with(""), prom_number(m.value));
                break;
            case Type::histogram:
                out += std::format("# TYPE {} histogram\n", n);
                for (std::size_t i = 0; i < m.bounds.size(); ++i) {
                    out += std::format("{}_bucket{} {}\n", n, with(std::format("le=\"{}\"", prom_number(m.bounds[i]))),
                                       m.buckets[i]);
                }
                out += std::format("{}_bucket{} {}\n", n, with("le=\"+Inf\""), m.count);
                out +=
                    std::format("{}_sum{} {}\n{}_count{} {}\n", n, with(""), prom_number(m.sum), n, with(""), m.count);
                break;
        }
    }
    return out;
}

Registry& registry() {
    static Registry r;
    return r;
}

}  // namespace forge::metrics
