// pump: the application the tutorials (docs/tutorials) build step by step.
// A simulated pump with a speed setpoint, start/stop, a pressure reading, an
// overpressure alarm with hysteresis, a command with parameters, a stream of
// readings and heavy work done off the main loop.
#pragma once

#include <forge/forge.hpp>

#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

class Pump : public forge::Application {
public:
    Pump() : Application({"pump", "1.0.0", "Tutorial pump controller"}) {}

    // State: on the main loop only (docs/threading.md).
    double speed_rpm = 1200;    ///< setpoint
    bool running = false;
    double pressure_bar = 0;    ///< simulated reading
    double limit_bar = 8;       ///< overpressure limit
    std::vector<double> history;

protected:
    // [pump] in pump.toml; defaults live here only.
    void setup_config(forge::config::Schema& s) override {
        s.section("pump", "The pump");
        s.number("pump.limit_bar", 8.0, "Overpressure alarm limit").range(1, 20).unit("bar");
        s.integer("pump.sample_ms", 100, "Pressure sampling period").range(10, 10000).unit("ms");
        s.integer("pump.alarm_after_s", 3, "Overpressure this long raises the alarm").range(0, 600).unit("s");
    }

    // One declaration: web panel, forgectl, forge.v1, console and telnet.
    void setup_web(forge::web::Dashboard& ui) override {
        auto& drive = ui.group("Drive");
        drive.number("speed_rpm", "Speed", speed_rpm).range(0, 3000, 10).unit("rpm").persist();
        drive.value("running", "Running", running);
        drive.button("start", "Start", [this] { running = true; }).primary();
        drive.button("stop", "Stop", [this] { running = false; }).danger().confirm("Stop the pump?");

        auto& readings = ui.group("Readings");
        readings.value("pressure_bar", "Pressure", [this] { return std::round(pressure_bar * 100) / 100; })
            .unit("bar")
            .precision(2);
        readings.number("limit_bar", "Alarm limit", limit_bar).range(1, 20, 0.1).unit("bar");

        // A command with typed parameters: forgectl call flush seconds=5
        ui.group("Service").command("flush", "Flush", {forge::web::number_param("seconds", 10).range(1, 60)},
                                    [this](const forge::json& a) {
                                        running = true;
                                        const auto s = a["seconds"].get<double>();
                                        after(std::chrono::milliseconds(static_cast<int>(s * 1000)),
                                              [this] { running = false; });
                                        return forge::json{{"flushing_s", s}};
                                    });
        ui.group("Service").button("export", "Export history", [this] { export_history(); });
    }

    void on_start() override {
        const auto cfg = config();
        limit_bar = cfg.value<double>("pump.limit_bar");
        alarm_after_ = std::chrono::seconds(cfg.value<int>("pump.alarm_after_s"));
        readings_ = &stream("pressure", "forge.v1.Values");
        every(std::chrono::milliseconds(cfg.value<int>("pump.sample_ms")), [this] { sample(); });
    }

private:
    void sample() {
        // The simulated pressure follows the speed; a real pump would read a sensor
        // on its own thread and post() the value here.
        const double target = running ? speed_rpm / 250.0 : 0.0;
        pressure_bar += (target - pressure_bar) * 0.2;
        history.push_back(pressure_bar);
        if (history.size() > 10000) history.erase(history.begin());

        // Overpressure for alarm_after_ (hysteresis): read time with now(), so a
        // test can drive it (AppRunner {.manual_time = true}).
        if (pressure_bar > limit_bar) {
            if (!over_since_) over_since_ = now();
            if (now() - *over_since_ >= alarm_after_) {
                alarms().raise("pump.overpressure", forge::Severity::error,
                               fmt::format("Pressure {:.1f} bar above {:.1f} bar", pressure_bar, limit_bar));
            }
        } else if (pressure_bar < limit_bar * 0.95) {
            over_since_.reset();
            alarms().clear("pump.overpressure");
        }

        // The stream: only built when someone listens.
        if (readings_->wanted()) {
            forge::v1::Values v;
            (*v.mutable_values())["pressure_bar"] = forge::proto::to_value(pressure_bar);
            readings_->publish(v);
        }
    }

    // Slow work leaves the main loop: the copy is taken here, the file is
    // written on a worker, the result comes back to the loop.
    void export_history() {
        background([h = history] { return write_csv(h); },
                   [this](std::string file) { notify_web("History saved to " + file, "success"); });
    }
    static std::string write_csv(const std::vector<double>& h);

    std::optional<forge::Scheduler::Clock::time_point> over_since_;
    std::chrono::seconds alarm_after_{3};
    forge::zmq::Stream* readings_ = nullptr;
};
