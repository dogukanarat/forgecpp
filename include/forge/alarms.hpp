#pragma once
// Alarm management, the way control systems (HMI/SCADA) do it.
//
// An alarm is identified by a stable id. Raising an alarm that is already
// active - or that cleared but was not acknowledged yet - only updates it
// (message, last time, occurrence count): operators are not flooded with
// duplicates by a flapping condition. Every alarm goes through:
//
//   raise() -> active, unacknowledged   (banner + bell badge in the web UI)
//   acknowledge() -> operator has seen it
//   clear() -> condition is gone        (kept in the history list)
//
//   app.alarms().raise("tank.level", forge::Severity::warning, "Tank level 92 %");
//   app.alarms().clear("tank.level");
//
// Thread-safe: raise/clear from any thread.

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace forge {

enum class Severity { info, warning, error, critical };

const char* to_string(Severity severity);
/// Parse "info|warning|error|critical"; throws std::invalid_argument.
Severity parse_severity(std::string_view text);

struct Alarm {
    using Clock = std::chrono::system_clock;

    std::string id;
    Severity severity = Severity::warning;
    std::string message;
    bool active = true;
    bool acknowledged = false;
    std::string acknowledged_by;
    std::uint64_t count = 1;  ///< raises since it became active
    Clock::time_point raised_at;
    Clock::time_point last_at;
    std::optional<Clock::time_point> cleared_at;
    std::optional<Clock::time_point> acknowledged_at;

    /// Needs operator attention: active and not yet acknowledged.
    bool pending() const { return active && !acknowledged; }
};

class Alarms {
public:
    /// Called after every change (from the thread that made it).
    using Listener = std::function<void()>;

    explicit Alarms(std::size_t history = 50);

    Alarms(const Alarms&) = delete;
    Alarms& operator=(const Alarms&) = delete;

    /// Raise or refresh an alarm. Returns true when it is new or became
    /// active again (i.e. operators should be told), false for a repeat.
    bool raise(const std::string& id, Severity severity, const std::string& message);
    /// The condition is gone. Returns false if the alarm was not active.
    bool clear(const std::string& id);
    /// Raise when `condition` is true, clear otherwise (handy in periodic checks).
    bool set(const std::string& id, bool condition, Severity severity, const std::string& message);

    /// Operator acknowledgement. Returns false for unknown ids.
    bool acknowledge(const std::string& id, const std::string& by = {});
    std::size_t acknowledge_all(const std::string& by = {});
    /// Drop cleared alarms from the history.
    void purge_cleared();

    std::vector<Alarm> list() const;
    std::optional<Alarm> get(const std::string& id) const;
    bool active(const std::string& id) const;
    std::size_t active_count() const;
    std::size_t pending_count() const;
    /// Highest severity among active alarms.
    std::optional<Severity> highest_active() const;

    /// Alarms ordered for display: pending first, then active, then history;
    /// most severe and most recent first within each.
    nlohmann::json to_json() const;
    /// {"active": n, "pending": n, "highest": "error"|null}
    nlohmann::json summary() const;

    void on_change(Listener listener);

private:
    void notify();
    void trim_history_locked();

    mutable std::mutex mutex_;
    std::vector<Alarm> alarms_;
    std::size_t history_;
    std::vector<Listener> listeners_;
};

nlohmann::json to_json(const Alarm& alarm);

}  // namespace forge
