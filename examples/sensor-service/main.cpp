// A complete forgecpp service (subclass style) showing every building block:
//
//   * CLI11 options and TOML config with live reload (SIGHUP / `reload`)
//   * a producer thread pushing samples through a lock-free forge::Channel
//   * batch processing on the forge::ThreadPool
//   * typed events on the forge::EventBus
//   * a web control panel (forge::web::Dashboard) at http://localhost:8080/forge/
//   * its own web page (web/index.html, compiled into the binary) at http://localhost:8080/
//   * a JSON REST API with forge::http::Server
//   * interactive console commands with completion (-i) as a backup
//
//   ./sensor-service -c sensor-service.toml -i
//   curl localhost:8080/api/stats
//   curl -X POST localhost:8080/forge/api/controls/rate_hz -d '{"value": 50}'

#include <forge/forge.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <random>

using namespace std::chrono_literals;
using forge::json;

namespace {

struct Sample {
    std::chrono::system_clock::time_point time;
    double value = 0.0;
};

struct Stats {
    std::size_t count = 0;
    double min = 0, max = 0, mean = 0;
};

/// Published on the event bus whenever a window mean is above the threshold.
struct ThresholdExceeded {
    double mean;
    double threshold;
};

Stats compute(const std::vector<Sample>& window) {
    Stats s;
    if (window.empty()) return s;
    s.count = window.size();
    s.min = s.max = window.front().value;
    double sum = 0;
    for (const auto& x : window) {
        s.min = std::min(s.min, x.value);
        s.max = std::max(s.max, x.value);
        sum += x.value;
    }
    s.mean = sum / static_cast<double>(window.size());
    return s;
}

json to_json(const Stats& s) { return {{"count", s.count}, {"min", s.min}, {"max", s.max}, {"mean", s.mean}}; }

}  // namespace

class SensorService : public forge::Application {
public:
    SensorService() : Application({"sensor-service", "1.0.0", "Example forgecpp sensor service"}) {}

protected:
    void setup_config(forge::config::Schema& s) override {
        s.section("sensor", "Simulated sensor");
        s.number("sensor.rate_hz", 10.0, "Samples per second").range(0.1, 10000).unit("Hz");
        s.integer("sensor.window", 50, "Samples per statistics window").range(1, 100000);
        s.number("sensor.alarm_threshold", 80.0, "Alarm when the window mean exceeds this");
    }

    void setup_cli(CLI::App& cli) override {
        cli.add_flag_callback("--simulate-spikes", [this] { spikes_ = true; },
                              "Inject random spikes to trigger alarms");
    }

    void setup_http(forge::http::Server& server) override {
        // HTTP handlers run on server threads; hop to the main loop to read state.
        server.get_json("/api/stats", [this](const auto&) { return invoke([this] { return stats_json(); }); });
        server.get_json("/api/rate", [this](const auto&) { return json{{"rate_hz", rate_hz_.load()}}; });
        server.put_json("/api/rate", [this](const auto&, const json& body) {
            const double hz = body.at("rate_hz").get<double>();
            if (hz <= 0 || hz > 1000) throw forge::http::HttpError(400, "rate_hz must be in (0, 1000]");
            rate_hz_ = hz;
            return json{{"rate_hz", hz}};
        });
    }

    // Web control panel: every control also works from the console
    // (`controls`, `set rate_hz 50`, `press reset`).
    void setup_web(forge::web::Dashboard& ui) override {
        ui.link("Live view", "/");

        auto& live = ui.group("Live");
        live.value("mean", "Window mean", [this] { return last_.mean; }).precision(1);
        live.value("min", "Window min", [this] { return last_.min; }).precision(1);
        live.value("max", "Window max", [this] { return last_.max; }).precision(1);
        live.value("samples", "Samples", total_);
        live.value("exceedances", "Threshold exceedances", exceedances_);

        auto& acq = ui.group("Acquisition");
        acq.slider("rate_hz", "Sampling rate", [this] { return rate_hz_.load(); }, [this](double hz) { rate_hz_ = hz; },
                   1, 500, 1)
            .unit("Hz");
        acq.number("window", "Window size", window_size_).range(5, 1000, 1).unit("samples");
        acq.toggle("spikes", "Simulate spikes", [this] { return spikes_.load(); }, [this](bool on) { spikes_ = on; });

        auto& alarm = ui.group("Alarm");
        alarm.toggle("alarm_enabled", "High-mean alarm", alarm_enabled_)
            .help("Raised while the window mean is above the threshold, cleared below 95 % of it");
        alarm.slider("threshold", "Threshold", threshold_, 0, 150, 0.5).precision(1);
        alarm.select("alarm_severity", "Severity", {"warning", "error", "critical"}, alarm_severity_);
        alarm.button("test_alarm", "Test alarm", [this] {
            alarms().raise("sensor.test", forge::Severity::info, "Test alarm raised from the dashboard");
        });
        alarm
            .button("reset", "Reset counters",
                    [this] {
            total_ = 0;
            exceedances_ = 0;
        })
            .confirm("Reset the sample and alarm counters?")
            .danger();
    }

    void setup_console(forge::Console& console) override {
        console.add("stats", "stats  show the latest statistics window",
                    [this, &console](const forge::Console::Args&) { console.print("{}", stats_json().dump(2)); });
        console.add("rate", "rate [hz]  show or change the sampling rate",
                    [this, &console](const forge::Console::Args& args) {
            if (!args.empty()) rate_hz_ = std::stod(args[0]);
            console.print("rate: {} Hz", rate_hz_.load());
        }, [](const forge::Console::Args& so_far) -> std::vector<std::string> {
            if (!so_far.empty()) return {};
            return {"1", "10", "50", "100"};
        });
    }

    void on_start() override {
        apply_config(config());

        // Events decouple detection (on_window) from reaction (alarms, counters).
        // Raising an alarm that is already active only updates it, so the
        // operator sees one entry with a counter, not a flood of messages.
        threshold_sub_ = events().subscribe<ThresholdExceeded>([this](const ThresholdExceeded& e) {
            ++exceedances_;
            if (!alarm_enabled_) return;
            alarms().raise("sensor.high_mean", forge::parse_severity(alarm_severity_),
                           std::format("Window mean {:.1f} exceeds threshold {:.1f}", e.mean, e.threshold));
        });

        // Producer: an independent thread that never touches application state
        // (stopped and joined before on_stop()).
        worker("producer", [this](std::stop_token stop) { produce(stop); });

        // Consumer: every 100 ms drain the channel on the main loop, and hand
        // full windows to the thread pool for (pretend) heavy processing.
        every(100ms, [this] { drain(); });
        every(10s, [this] { logger()->info("stats: {}", stats_json().dump()); });
    }

    void on_reload(const forge::Config& cfg) override { apply_config(cfg); }

    void on_stop() override { samples_.close(); }

private:
    void apply_config(const forge::Config& cfg) {
        // Declared in setup_config(): the defaults come from there.
        rate_hz_ = cfg.value<double>("sensor.rate_hz");
        window_size_ = cfg.value<std::size_t>("sensor.window");
        threshold_ = cfg.value<double>("sensor.alarm_threshold");
        logger()->info("sensor: {} Hz, window {}, alarm > {}", rate_hz_.load(), window_size_, threshold_);
    }

    void produce(const std::stop_token& stop) {
        std::mt19937 rng{std::random_device{}()};
        std::normal_distribution<double> noise(0.0, 3.0);
        std::uniform_real_distribution<double> spike(0.0, 1.0);
        double t = 0;
        while (!stop.stop_requested()) {
            double value = 50.0 + 20.0 * std::sin(t) + noise(rng);
            if (spikes_ && spike(rng) < 0.05) value += 60.0;
            samples_.push(Sample{std::chrono::system_clock::now(), value});
            t += 0.05;
            forge::sleep_for(stop, std::chrono::duration<double>(1.0 / rate_hz_.load()));
        }
    }

    void drain() {
        while (auto item = samples_.try_pop()) {
            window_.push_back(*item);
            ++total_;
            if (window_.size() >= window_size_) {
                // Process the full window on a worker thread, publish the result back.
                pool().detach_task([this, w = std::move(window_)] {
                    auto stats = compute(w);
                    post([this, stats] { on_window(stats); });
                });
                window_.clear();
            }
        }
    }

    void on_window(const Stats& stats) {
        last_ = stats;
        logger()->debug("window: mean {:.2f} min {:.2f} max {:.2f}", stats.mean, stats.min, stats.max);
        if (stats.mean > threshold_) {
            emit(ThresholdExceeded{stats.mean, threshold_});
        } else if (stats.mean < threshold_ * 0.95 || !alarm_enabled_) {
            alarms().clear("sensor.high_mean");  // hysteresis: no flapping around the threshold
        }
    }

    json stats_json() const {
        return {{"total_samples", total_},
                {"exceedances", exceedances_},
                {"rate_hz", rate_hz_.load()},
                {"last_window", to_json(last_)}};
    }

    // Configuration
    std::atomic<double> rate_hz_{10.0};
    std::size_t window_size_ = 50;
    double threshold_ = 80.0;
    bool alarm_enabled_ = true;
    std::string alarm_severity_ = "warning";
    std::atomic<bool> spikes_{false};

    // Producer side
    forge::Channel<Sample> samples_;

    // Main loop state (only touched on the main loop)
    std::vector<Sample> window_;
    std::size_t total_ = 0;
    std::size_t exceedances_ = 0;
    Stats last_;
    forge::EventBus::Subscription threshold_sub_;
};

FORGE_MAIN(SensorService)
