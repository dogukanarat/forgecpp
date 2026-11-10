// three-threads: a forgecpp daemon with three threads of its own.
//
//   acquire  --Channel<Sample>-->  process  --Channel<Block>-->  downlink
//                                     |
//                                   post()
//                                     v
//                               main loop (state, controls, alarms)
//
//   acquire   reads a (dummy) sensor at [demo].rate_hz
//   process   averages every [demo].block samples into a block
//   downlink  sends the blocks once a second (here: logs them)
//
// The rule of forgecpp: the application's state belongs to the main loop.
// The threads hand data to each other through forge::Channel, and to the
// main loop through post(); what the panel changes and a thread reads is an
// std::atomic.
//
//   ./three-threads --http 8080 -i     # panel: http://localhost:8080/forge/
//   ./three-threads -D demo.rate_hz=500
//
// Thread priorities and CPUs come from the configuration, by thread name:
//
//   [threads.acquire]
//   policy = "fifo"
//   priority = 50
//   cpus = [1]

#include <forge/forge.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;

struct Sample {
    std::uint64_t seq = 0;
    double value = 0;
};

struct Block {
    std::uint64_t first_seq = 0;
    double mean = 0, min = 0, max = 0;
};

class ThreeThreads : public forge::Application {
public:
    ThreeThreads() : Application({"three-threads", "1.0.0", "Three threads and a main loop"}) {}

protected:
    void setup_config(forge::config::Schema& s) override {
        s.section("demo", "The dummy sensor");
        s.integer("demo.rate_hz", 100, "Samples per second").range(1, 10000).unit("Hz");
        s.integer("demo.block", 10, "Samples per block").range(1, 10000);
    }

    void setup_controls(forge::Controls& ui) override {
        auto& in = ui.group("Acquire");
        in.number("rate_hz", "Rate", rate_hz_).range(1, 10000).unit("Hz");  // the thread reads it
        in.value("acquired", "Samples", acquired_);

        auto& proc = ui.group("Process");
        proc.value("blocks", "Blocks", blocks_);
        proc.value("mean", "Last mean", last_mean_).precision(3);  // main loop state
        proc.value("queued", "Waiting samples", [this] { return samples_.size_approx(); });

        auto& out = ui.group("Downlink");
        out.value("sent", "Blocks sent", sent_);
        out.toggle("fail", "Simulate a fault", fail_downlink_);
    }

    void on_start() override {
        const auto cfg = config();
        rate_hz_ = cfg.value<int>("demo.rate_hz");
        block_size_ = cfg.value<std::size_t>("demo.block");

        // worker(): a named thread; [threads.<name>] is applied to it, it is
        // stopped before on_stop(), and if its body throws, health and an
        // alarm say so (and the systemd watchdog restarts the program).
        worker("acquire", [this](std::stop_token stop) { acquire(stop); });
        worker("process", [this](std::stop_token stop) { process(stop); });
        worker("downlink", [this](std::stop_token stop) { downlink(stop); });

        every(5s, [this] {  // on the main loop
            logger()->info("acquired {}, blocks {}, sent {}, last mean {:.3f}", acquired_.load(), blocks_.load(),
                           sent_.load(), last_mean_);
        });
    }

private:
    // --- thread 1: acquire ---------------------------------------------------
    void acquire(const std::stop_token& stop) {
        std::mt19937 rng(42);
        std::normal_distribution<double> noise(0.0, 0.1);
        std::uint64_t seq = 0;
        auto next = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            const double t = static_cast<double>(seq) / 100.0;
            samples_.push(Sample{seq++, std::sin(t) + noise(rng)});  // the dummy sensor
            acquired_.fetch_add(1, std::memory_order_relaxed);

            next += std::chrono::microseconds(1'000'000 / std::max(rate_hz_.load(), 1));
            forge::sleep_for(stop, next - std::chrono::steady_clock::now());  // returns at once on stop
        }
    }

    // --- thread 2: process ---------------------------------------------------
    void process(const std::stop_token& stop) {
        std::vector<Sample> batch;
        batch.reserve(block_size_);
        while (!stop.stop_requested()) {
            auto s = samples_.pop_for(100ms);  // wakes up to see the stop request
            if (!s) continue;
            batch.push_back(*s);
            if (batch.size() < block_size_) continue;

            Block b{batch.front().seq, 0, batch.front().value, batch.front().value};
            for (const auto& x : batch) {
                b.mean += x.value;
                b.min = std::min(b.min, x.value);
                b.max = std::max(b.max, x.value);
            }
            b.mean /= static_cast<double>(batch.size());
            batch.clear();

            blocks_out_.push(b);
            blocks_.fetch_add(1, std::memory_order_relaxed);
            post([this, mean = b.mean] { last_mean_ = mean; });  // into the main loop
        }
    }

    // --- thread 3: downlink --------------------------------------------------
    void downlink(const std::stop_token& stop) {
        std::vector<Block> ready(64);
        while (!forge::sleep_for(stop, 1s)) {
            if (fail_downlink_.load()) throw std::runtime_error("radio does not answer");
            std::size_t n;
            while ((n = blocks_out_.pop_bulk(ready.begin(), ready.size(), 0ms)) > 0) {
                for (std::size_t i = 0; i < n; ++i) {
                    logger()->debug("downlink: block {} mean {:.3f} [{:.3f}, {:.3f}]", ready[i].first_seq,
                                    ready[i].mean, ready[i].min, ready[i].max);
                }
                sent_.fetch_add(n, std::memory_order_relaxed);
            }
        }
    }

    // Between the threads: lock-free queues.
    forge::Channel<Sample> samples_{1024};
    forge::Channel<Block> blocks_out_{256};

    // Written by one thread, read by the panel: atomics.
    std::atomic<int> rate_hz_{100};
    std::atomic<std::uint64_t> acquired_{0}, blocks_{0}, sent_{0};
    std::atomic<bool> fail_downlink_{false};

    // Main loop only: no lock, no atomic.
    double last_mean_ = 0;
    std::size_t block_size_ = 10;  // set in on_start(), before the threads start
};

FORGE_MAIN(ThreeThreads)
