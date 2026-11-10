// Multi-threaded processing pipeline built with forgecpp.
//
//   producers (N threads) --Channel<Job>--> dispatcher thread --> ThreadPool workers
//        ^                                                              |
//        | rate / size / pause (web, console)                Channel<Result>
//        |                                                              v
//   web dashboard  <------------------  main loop (stats, alarms)  <-----+
//
// What it shows:
//   * long-lived threads you own (producers, dispatcher) with clean start/stop
//   * lock-free MPMC hand-off with forge::Channel (bulk pops)
//   * backpressure: bounded in-flight work, backlog alarm with hysteresis
//   * CPU work spread over forge::ThreadPool
//   * std::atomic for values read by the web UI while threads update them
//   * results folded into state on the main loop only (no locks needed)
//   * alarms raised straight from worker threads (forge::Alarms is thread-safe)
//
//   ./pipeline --producers 4 --http 8080 -i
//   open http://localhost:8080/forge/

#include <forge/forge.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

struct Job {
    std::uint64_t id = 0;
    std::uint32_t size = 0;  ///< amount of work (iterations)
    Clock::time_point created;
};

struct Result {
    std::uint64_t id = 0;
    bool ok = true;
    std::uint64_t checksum = 0;
    double latency_ms = 0;  ///< created -> finished
};

/// Pretend CPU work: a few thousand rounds of integer mixing.
std::uint64_t crunch(std::uint64_t seed, std::uint32_t rounds) {
    std::uint64_t x = seed * 0x9E3779B97F4A7C15ULL + 1;
    for (std::uint32_t i = 0; i < rounds; ++i) {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 29;
    }
    return x;
}

/// Rate over a sliding window of one-second buckets.
class RateMeter {
public:
    void add(std::uint64_t n) { current_ += n; }
    void tick() {  // call once per second
        history_[pos_++ % history_.size()] = current_;
        current_ = 0;
    }
    double per_second() const {
        const auto n = std::min<std::size_t>(pos_, history_.size());
        if (n == 0) return 0;
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < n; ++i) sum += history_[i];
        return static_cast<double>(sum) / static_cast<double>(n);
    }

private:
    std::array<std::uint64_t, 5> history_{};
    std::size_t pos_ = 0;
    std::uint64_t current_ = 0;
};

}  // namespace

class Pipeline : public forge::Application {
public:
    Pipeline() : Application({"pipeline", "1.0.0", "Multi-threaded processing pipeline example"}) {}

protected:
    void setup_config(forge::config::Schema& s) override {
        s.section("pipeline", "Processing pipeline");
        s.integer("pipeline.producers", 2, "Producer threads (--producers wins)").range(1, 64);
        s.integer("pipeline.rate", 200, "Jobs per second per producer").range(1, 1000000).unit("jobs/s");
        s.integer("pipeline.job_size", 20000, "Work per job").range(1, 100000000);
    }

    void setup_cli(CLI::App& cli) override {
        cli.add_option("--producers", producer_count_, "Number of producer threads")->check(CLI::Range(1, 64));
    }

    void setup_controls(forge::Controls& ui) override {
        ui.title("Processing pipeline");

        auto& flow = ui.group("Throughput");
        flow.value("produced_per_s", "Produced", [this] { return produced_rate_.per_second(); })
            .unit("jobs/s")
            .precision(0);
        flow.value("done_per_s", "Completed", [this] { return done_rate_.per_second(); }).unit("jobs/s").precision(0);
        flow.value("queue", "Queue depth", [this] { return input_.size_approx(); }).unit("jobs");
        flow.value("in_flight", "In flight", [this] { return in_flight_.load(); }).unit("jobs");
        flow.value("latency_avg", "Latency (avg)", latency_avg_).unit("ms").precision(1);
        flow.value("latency_max", "Latency (max, 1 s)", latency_max_).unit("ms").precision(1);
        flow.value("completed", "Completed total", completed_);
        flow.value("failed", "Failed total", failed_);

        auto& prod = ui.group("Producers");
        prod.value("producers", "Threads", producer_count_);
        prod.toggle("paused", "Paused", [this] { return paused_.load(); }, [this](bool v) { paused_ = v; });
        prod.slider("rate", "Rate per producer", [this] { return double(rate_per_producer_.load()); },
                    [this](double v) { rate_per_producer_ = static_cast<int>(v); }, 1, 5000, 1)
            .unit("jobs/s");
        prod.slider("job_size", "Job size", [this] { return double(job_size_.load()); },
                    [this](double v) { job_size_ = static_cast<std::uint32_t>(v); }, 100, 200000, 100)
            .unit("rounds");
        prod.button("burst", "Burst 10 000 jobs", [this] { burst(10000); }).primary();

        auto& work = ui.group("Workers");
        work.value("workers", "Pool threads", [this] { return pool().get_thread_count(); });
        work.slider("failure_rate", "Simulated failures", [this] { return failure_rate_.load() * 100; },
                    [this](double pct) { failure_rate_ = pct / 100; }, 0, 50, 0.5)
            .unit("%")
            .precision(1);
        work.number("queue_limit", "Backlog alarm at", [this] { return double(queue_limit_.load()); },
                    [this](double v) { queue_limit_ = static_cast<std::size_t>(v); })
            .range(100, 1000000, 100)
            .unit("jobs");
        work.button("reset", "Reset counters", [this] {
            completed_ = failed_ = 0;
            latency_avg_ = latency_max_ = 0;
        }).confirm("Reset all counters?");
    }

    void setup_console(forge::Console& console) override {
        console.add("burst", "burst [n]  enqueue n jobs at once (default 10000)",
                    [this, &console](const forge::Console::Args& a) {
            const auto n = a.empty() ? 10000 : std::stoul(a[0]);
            burst(n);
            console.print("queued {} jobs", n);
        });
    }

    void on_start() override {
        const auto cfg = config();
        if (producer_count_ == 0) producer_count_ = cfg.value<int>("pipeline.producers");
        rate_per_producer_ = cfg.value<int>("pipeline.rate");
        job_size_ = cfg.value<std::uint32_t>("pipeline.job_size");

        for (int i = 0; i < producer_count_; ++i) {
            producers_.emplace_back([this, i] { produce(i); });
        }
        dispatcher_ = std::thread([this] { dispatch(); });

        every(100ms, [this] { collect(); });  // fold results into state (main loop)
        every(1s, [this] { second_tick(); });
        logger()->info("pipeline: {} producers, {} workers", producer_count_, pool().get_thread_count());
    }

    void on_stop() override {
        running_ = false;
        input_.close();
        for (auto& t : producers_) t.join();
        dispatcher_.join();
        pool().wait();  // let in-flight jobs finish
    }

private:
    // --- producer threads ----------------------------------------------------
    void produce(int index) {
        std::mt19937 rng(static_cast<unsigned>(index) * 7919u + 1);
        std::uniform_real_distribution<double> jitter(0.8, 1.2);
        while (running_) {
            if (paused_) {
                std::this_thread::sleep_for(50ms);
                continue;
            }
            const auto size = static_cast<std::uint32_t>(job_size_.load() * jitter(rng));
            input_.push(Job{next_id_++, size, Clock::now()});
            produced_ += 1;
            std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / std::max(1, rate_per_producer_.load())));
        }
    }

    void burst(std::size_t n) {
        std::vector<Job> jobs(n);
        const auto now = Clock::now();
        for (auto& j : jobs) j = Job{next_id_++, job_size_.load(), now};
        input_.push_bulk(jobs.begin(), jobs.size());
        produced_ += n;
        logger()->info("burst: {} jobs queued", n);
    }

    // --- dispatcher thread -----------------------------------------------------
    void dispatch() {
        std::vector<Job> batch(256);
        // Bound the work handed to the pool: jobs that cannot be processed yet
        // stay in the channel, where the backlog is visible and measurable.
        const auto max_in_flight = static_cast<std::int64_t>(pool().get_thread_count() * 4);
        auto next_check = Clock::now();
        while (running_) {
            std::size_t n = 0;
            const auto room = max_in_flight - in_flight_.load();
            if (room > 0) {
                n = input_.pop_bulk(batch.begin(), std::min<std::size_t>(batch.size(), static_cast<std::size_t>(room)),
                                    100ms);
            } else {
                std::this_thread::sleep_for(1ms);
            }

            // Backpressure, checked once a second: the dispatcher raises and
            // clears the alarm itself - forge::Alarms is thread-safe.
            // Clearing at half the limit avoids flapping (hysteresis).
            if (Clock::now() >= next_check) {
                next_check = Clock::now() + 1s;
                const auto depth = input_.size_approx();
                const auto limit = queue_limit_.load();
                if (depth > limit) {
                    alarms().raise("pipeline.backlog", forge::Severity::warning,
                                   std::format("Job backlog {} exceeds {} - workers cannot keep up", depth, limit));
                } else if (depth < limit / 2) {
                    alarms().clear("pipeline.backlog");
                }
            }

            for (std::size_t i = 0; i < n; ++i) {
                ++in_flight_;
                pool().detach_task([this, job = batch[i]] { work(job); });
            }
        }
    }

    // --- worker threads (thread pool) -------------------------------------------
    void work(const Job& job) {
        thread_local std::mt19937 rng{std::random_device{}()};
        Result r;
        r.id = job.id;
        r.checksum = crunch(job.id, job.size);
        r.ok = std::uniform_real_distribution<double>(0, 1)(rng) >= failure_rate_.load();
        r.latency_ms = std::chrono::duration<double, std::milli>(Clock::now() - job.created).count();
        results_.push(r);
        --in_flight_;
    }

    // --- main loop ----------------------------------------------------------------
    void collect() {
        std::vector<Result> batch(1024);
        std::size_t n;
        while ((n = results_.pop_bulk(batch.begin(), batch.size(), 0ms)) > 0) {
            for (std::size_t i = 0; i < n; ++i) {
                const auto& r = batch[i];
                if (r.ok) {
                    ++completed_;
                } else {
                    ++failed_;
                    ++failed_window_;
                }
                ++done_window_;
                latency_sum_ += r.latency_ms;
                window_max_ = std::max(window_max_, r.latency_ms);
            }
            done_rate_.add(n);
        }
    }

    void second_tick() {
        produced_rate_.add(produced_.exchange(0));
        produced_rate_.tick();
        done_rate_.tick();
        if (done_window_ > 0) {
            latency_avg_ = latency_sum_ / static_cast<double>(done_window_);
            latency_max_ = window_max_;
            const double fail_pct = 100.0 * static_cast<double>(failed_window_) / static_cast<double>(done_window_);
            alarms().set("pipeline.failures", fail_pct > 10, forge::Severity::error,
                         std::format("{:.1f} % of jobs failed in the last second", fail_pct));
        }
        done_window_ = failed_window_ = 0;
        latency_sum_ = window_max_ = 0;
    }

    // Shared between threads
    std::atomic<bool> running_{true};
    std::atomic<bool> paused_{false};
    std::atomic<int> rate_per_producer_{200};
    std::atomic<std::uint32_t> job_size_{20000};
    std::atomic<double> failure_rate_{0.0};
    std::atomic<std::size_t> queue_limit_{5000};
    std::atomic<std::uint64_t> next_id_{1};
    std::atomic<std::uint64_t> produced_{0};
    std::atomic<std::int64_t> in_flight_{0};
    forge::Channel<Job> input_{4096};
    forge::Channel<Result> results_{4096};
    std::vector<std::thread> producers_;
    std::thread dispatcher_;
    int producer_count_ = 0;

    // Main loop only
    RateMeter produced_rate_, done_rate_;
    std::uint64_t completed_ = 0, failed_ = 0;
    std::uint64_t done_window_ = 0, failed_window_ = 0;
    double latency_sum_ = 0, window_max_ = 0;
    double latency_avg_ = 0, latency_max_ = 0;
};

FORGE_MAIN(Pipeline)
