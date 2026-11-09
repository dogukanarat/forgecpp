#pragma once
// Component health. Report from anywhere (thread-safe):
//
//   app.health().set("radio", forge::HealthState::failed, "device not found");
//   app.health().set("radio", forge::HealthState::ok);
//
// Shown in /health ("health" field; GET /health?strict=1 answers 503 while a
// critical component has failed), the control panel, the "health" command and
// the "health" event topic. With [health].watchdog_grace_s > 0, a critical
// component failed for longer than that stops the systemd watchdog pings,
// so systemd restarts the service (WatchdogSec in the unit).

#include "forge/json.hpp"
#include "forge/subscription.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace forge {

enum class HealthState { ok = 1, degraded = 2, failed = 3 };

const char* to_string(HealthState s);

class Health {
public:
    struct Component {
        std::string name;
        HealthState state = HealthState::ok;
        std::string message;
        bool critical = true;
        std::int64_t since_unix_ms = 0;
    };
    struct Report {
        HealthState state = HealthState::ok;  ///< the worst of the components
        std::vector<Component> components;    ///< sorted by name
    };

    /// `critical`: a failure of this component means the service is broken.
    void set(const std::string& component, HealthState state, const std::string& message = {}, bool critical = true);
    void remove(const std::string& component);

    HealthState overall() const;
    Report snapshot() const;
    /// Seconds the longest-failed critical component has been failed (nullopt: none failed).
    std::optional<double> critical_failed_for() const;
    /// Called after a change, on the thread that made it.
    [[nodiscard]] Subscription on_change(std::function<void(const Report&)> listener) {
        return listeners_.add(std::move(listener));
    }

private:
    struct Entry {
        HealthState state = HealthState::ok;
        std::string message;
        bool critical = true;
        std::chrono::system_clock::time_point since;
        std::chrono::steady_clock::time_point since_steady;
    };
    mutable std::mutex mutex_;
    std::map<std::string, Entry> components_;
    Listeners<Report> listeners_;
};

json to_json(const Health::Report& r);

}  // namespace forge
