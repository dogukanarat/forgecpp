// A web-first application: the thermostat is operated from its own web pages
// (web/), compiled into the binary. The generic control panel (/forge/) and
// the console (-i) stay available as backups.
//
//   web/index.html     overview: live temperature, setpoint, mode, chart, alarms
//   web/settings.html  settings form backed by a custom REST API
//
//   ./thermostat -c thermostat.toml                  # http://localhost:8080/
//   ./thermostat -c thermostat.toml -D web.root=web  # edit pages without rebuilding
//
// What it shows:
//   * forge_add_app(... WEB web): multi-page site with its own CSS/JS/images
//   * data-forge-* bindings (values, radios, buttons) on custom pages
//   * custom JSON endpoints (/api/history, /api/settings) with validation
//   * publish_web(): pushing domain events ("reading") to the pages
//   * alarms with hysteresis and a fail-safe on sensor faults
//   * settings persisted atomically with forge::save_json

#include <forge/forge.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <random>

using namespace std::chrono_literals;
using forge::json;
using forge::http::HttpError;

namespace {

struct Settings {
    std::string room = "Living room";
    double hysteresis = 0.3;   ///< °C around the setpoint
    double min_setpoint = 10;
    double max_setpoint = 30;
    double high_alarm = 28;    ///< °C
};

json to_json(const Settings& s) {
    return {{"room", s.room},
            {"hysteresis", s.hysteresis},
            {"min_setpoint", s.min_setpoint},
            {"max_setpoint", s.max_setpoint},
            {"high_alarm", s.high_alarm}};
}

/// Validate user input; every problem is reported per field.
Settings settings_from_json(const json& j, Settings base) {
    json errors = json::object();
    auto number = [&](const char* key, double lo, double hi, double& out) {
        if (!j.contains(key)) return;
        if (!j[key].is_number()) {
            errors[key] = "must be a number";
            return;
        }
        const double v = j[key].get<double>();
        if (v < lo || v > hi) {
            errors[key] = std::format("must be between {} and {}", lo, hi);
            return;
        }
        out = v;
    };
    if (j.contains("room")) {
        if (!j["room"].is_string() || j["room"].get<std::string>().empty() ||
            j["room"].get<std::string>().size() > 40) {
            errors["room"] = "1 to 40 characters";
        } else {
            base.room = j["room"].get<std::string>();
        }
    }
    number("hysteresis", 0.1, 2.0, base.hysteresis);
    number("min_setpoint", 5, 25, base.min_setpoint);
    number("max_setpoint", 15, 35, base.max_setpoint);
    number("high_alarm", 20, 45, base.high_alarm);
    if (!errors.contains("min_setpoint") && !errors.contains("max_setpoint") &&
        base.min_setpoint >= base.max_setpoint) {
        errors["max_setpoint"] = "must be above the minimum setpoint";
    }
    if (!errors.empty())
        throw HttpError(422, "invalid settings", json{{"error", "invalid settings"}, {"fields", errors}});
    return base;
}

}  // namespace

class Thermostat : public forge::Application {
public:
    Thermostat() : Application({"thermostat", "1.0.0", "Web-first thermostat example"}) {}

protected:
    void setup_config(forge::config::Schema& s) override {
        s.section("thermostat", "Thermostat");
        s.string("thermostat.settings_file", "thermostat-settings.json", "Settings saved from the settings page");
        s.number("thermostat.setpoint", 21.0, "Initial setpoint").unit("°C");
    }

    // Controls shared by the custom pages (data-forge-*), /forge/ and the console.
    void setup_controls(forge::Controls& ui) override {
        ui.title("Thermostat");
        ui.link("Overview", "/");
        ui.link("Settings", "/settings.html");

        auto& climate = ui.group("Climate");
        climate.value("room_name", "Room", [this] { return settings_.room; });
        climate.value("temperature", "Temperature", [this] { return fault_ ? json(nullptr) : json(room_); })
            .unit("°C")
            .precision(1);
        climate.value("outside", "Outside", outside_).unit("°C").precision(1);
        climate.value("status", "Status", [this] { return status(); });
        climate
            .slider("setpoint", "Setpoint", [this] { return setpoint_; }, [this](double v) { set_setpoint(v); }, 5, 35,
                    0.5)
            .unit("°C")
            .precision(1);
        climate.select("mode", "Mode", {"off", "heat", "cool", "auto"}, mode_);

        auto& sim = ui.group("Simulation");
        sim.toggle("sensor_fault", "Simulate sensor fault", fault_)
            .help("The controller must fail safe: heating and cooling stop");
        sim.slider("outside_offset", "Outside temperature offset", outside_offset_, -15, 15, 0.5).unit("°C");
    }

    void setup_http(forge::http::Server& server) override {
        server.get_json("/api/history", [this](const forge::http::Request&) {
            return invoke([this] {
                json out = json::array();
                for (const auto& p : history_) out.push_back(p);
                return out;
            });
        });
        server.get_json("/api/settings",
                        [this](const forge::http::Request&) { return invoke([this] { return to_json(settings_); }); });
        server.put_json("/api/settings", [this](const forge::http::Request& req, const json& body) {
            return invoke([&] {
                settings_ = settings_from_json(body, settings_);  // throws 422 with field errors
                set_setpoint(setpoint_);                          // re-clamp to the new limits
                save_settings();
                logger()->info("settings changed by {}: {}", req.remote_addr, to_json(settings_).dump());
                notify_web("Settings saved", "success");
                return to_json(settings_);
            });
        });
    }

    void on_start() override {
        const auto cfg = config();
        settings_file_ = cfg.value<std::string>("thermostat.settings_file");
        load_settings();
        setpoint_ =
            std::clamp(cfg.value<double>("thermostat.setpoint"), settings_.min_setpoint, settings_.max_setpoint);
        every(1s, [this] { step(); });
    }

private:
    void set_setpoint(double v) {
        setpoint_ = std::clamp(std::round(v * 2) / 2, settings_.min_setpoint, settings_.max_setpoint);
    }

    std::string status() const {
        if (fault_) return "sensor fault";
        if (heating_) return "heating";
        if (cooling_) return "cooling";
        return mode_ == "off" ? "off" : "idle";
    }

    // One simulation + control step per second (main loop).
    void step() {
        ++t_;
        outside_ = 8 + 6 * std::sin(t_ / 120.0) + outside_offset_;
        room_ += (outside_ - room_) * 0.004 + (heating_ ? 0.12 : 0) - (cooling_ ? 0.12 : 0) + noise_(rng_);

        // Bang-bang control with hysteresis; fail safe on sensor faults.
        const double h = settings_.hysteresis;
        const bool can_heat = !fault_ && (mode_ == "heat" || mode_ == "auto");
        const bool can_cool = !fault_ && (mode_ == "cool" || mode_ == "auto");
        if (!can_heat || room_ > setpoint_ + h)
            heating_ = false;
        else if (room_ < setpoint_ - h)
            heating_ = true;
        if (!can_cool || room_ < setpoint_ - h)
            cooling_ = false;
        else if (room_ > setpoint_ + h)
            cooling_ = true;

        alarms().set("sensor.fault", fault_, forge::Severity::error,
                     "Temperature sensor not responding - heating and cooling stopped");
        if (!fault_) {
            if (room_ > settings_.high_alarm) {
                alarms().raise(
                    "temperature.high", forge::Severity::warning,
                    std::format("{}: {:.1f} °C is above {:.1f} °C", settings_.room, room_, settings_.high_alarm));
            } else if (room_ < settings_.high_alarm - 0.5) {
                alarms().clear("temperature.high");
            }
        }

        json point{{"t", t_},
                   {"room", fault_ ? json(nullptr) : json(std::round(room_ * 100) / 100)},
                   {"setpoint", setpoint_},
                   {"outside", std::round(outside_ * 100) / 100},
                   {"heating", heating_},
                   {"cooling", cooling_}};
        history_.push_back(point);
        if (history_.size() > 600) history_.pop_front();
        publish_web("reading", point);  // custom pages draw their chart from this
    }

    void load_settings() {
        std::error_code ec;
        if (!std::filesystem::exists(settings_file_, ec)) return;
        try {
            settings_ = settings_from_json(forge::load_json(settings_file_), settings_);
            logger()->info("settings loaded from {}", settings_file_);
        } catch (const std::exception& e) {
            logger()->warn("ignoring {}: {}", settings_file_, e.what());
        }
    }

    void save_settings() {
        try {
            forge::save_json(settings_file_, to_json(settings_));
        } catch (const std::exception& e) {
            alarms().raise("settings.save", forge::Severity::warning, std::string("Cannot save settings: ") + e.what());
        }
    }

    // Main loop state
    Settings settings_;
    std::string settings_file_;
    double room_ = 19.0, outside_ = 8.0, outside_offset_ = 0, setpoint_ = 21.0;
    std::string mode_ = "auto";
    bool heating_ = false, cooling_ = false, fault_ = false;
    long t_ = 0;
    std::deque<json> history_;
    std::mt19937 rng_{42};
    std::normal_distribution<double> noise_{0.0, 0.02};
};

FORGE_MAIN(Thermostat)
