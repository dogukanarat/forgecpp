#include "forge/alarms.hpp"

#include "forge/log.hpp"

#include <algorithm>
#include <stdexcept>

namespace forge {
namespace {

std::int64_t epoch_ms(Alarm::Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

log::Level log_level(Severity s) {
    switch (s) {
        case Severity::info: return log::Level::info;
        case Severity::warning: return log::Level::warn;
        case Severity::error: return log::Level::error;
        case Severity::critical: return log::Level::critical;
    }
    return log::Level::warn;
}

// Display order: pending, then acknowledged-but-active, then history.
int group_of(const Alarm& a) { return a.pending() ? 0 : (a.active ? 1 : 2); }

}  // namespace

const char* to_string(Severity severity) {
    switch (severity) {
        case Severity::info: return "info";
        case Severity::warning: return "warning";
        case Severity::error: return "error";
        case Severity::critical: return "critical";
    }
    return "warning";
}

Severity parse_severity(std::string_view text) {
    if (text == "info") return Severity::info;
    if (text == "warning" || text == "warn") return Severity::warning;
    if (text == "error") return Severity::error;
    if (text == "critical") return Severity::critical;
    throw std::invalid_argument("unknown severity '" + std::string(text) + "' (info|warning|error|critical)");
}

nlohmann::json to_json(const Alarm& a) {
    nlohmann::json j{{"id", a.id},
                     {"severity", to_string(a.severity)},
                     {"message", a.message},
                     {"active", a.active},
                     {"acknowledged", a.acknowledged},
                     {"count", a.count},
                     {"raised_at", epoch_ms(a.raised_at)},
                     {"last_at", epoch_ms(a.last_at)}};
    if (a.cleared_at) j["cleared_at"] = epoch_ms(*a.cleared_at);
    if (a.acknowledged_at) j["acknowledged_at"] = epoch_ms(*a.acknowledged_at);
    if (!a.acknowledged_by.empty()) j["acknowledged_by"] = a.acknowledged_by;
    return j;
}

Alarms::Alarms(std::size_t history) : history_(history) {}

bool Alarms::raise(const std::string& id, Severity severity, const std::string& message) {
    if (id.empty()) throw std::invalid_argument("alarm id must not be empty");
    const auto now = Alarm::Clock::now();
    bool fresh = false;
    {
        std::lock_guard lock(mutex_);
        auto it = std::find_if(alarms_.begin(), alarms_.end(), [&](const Alarm& a) { return a.id == id; });
        if (it != alarms_.end() && it->active) {
            // Repeat of an active alarm: update in place, no new notification.
            ++it->count;
            it->last_at = now;
            it->message = message;
            if (severity > it->severity) {  // escalation needs attention again
                it->severity = severity;
                it->acknowledged = false;
                it->acknowledged_at.reset();
                it->acknowledged_by.clear();
                fresh = true;
            }
        } else if (it != alarms_.end() && !it->acknowledged) {
            // Returned to normal but nobody acknowledged it yet: a flapping
            // condition stays one entry with a growing counter.
            it->active = true;
            it->cleared_at.reset();
            ++it->count;
            it->last_at = now;
            it->message = message;
            if (severity > it->severity) it->severity = severity;
        } else {
            Alarm a;
            a.id = id;
            a.severity = severity;
            a.message = message;
            a.raised_at = a.last_at = now;
            if (it != alarms_.end()) alarms_.erase(it);
            alarms_.push_back(std::move(a));
            fresh = true;
        }
    }
    if (fresh) log::get().log(log_level(severity), "ALARM [{}] {}: {}", to_string(severity), id, message);
    notify();
    return fresh;
}

bool Alarms::clear(const std::string& id) {
    {
        std::lock_guard lock(mutex_);
        auto it = std::find_if(alarms_.begin(), alarms_.end(), [&](const Alarm& a) { return a.id == id; });
        if (it == alarms_.end() || !it->active) return false;
        it->active = false;
        it->cleared_at = Alarm::Clock::now();
        trim_history_locked();
    }
    log::info("alarm cleared: {}", id);
    notify();
    return true;
}

bool Alarms::set(const std::string& id, bool condition, Severity severity, const std::string& message) {
    return condition ? raise(id, severity, message) : clear(id);
}

bool Alarms::acknowledge(const std::string& id, const std::string& by) {
    {
        std::lock_guard lock(mutex_);
        auto it = std::find_if(alarms_.begin(), alarms_.end(), [&](const Alarm& a) { return a.id == id; });
        if (it == alarms_.end()) return false;
        if (it->acknowledged) return true;
        it->acknowledged = true;
        it->acknowledged_at = Alarm::Clock::now();
        it->acknowledged_by = by;
        trim_history_locked();
    }
    log::info("alarm acknowledged: {}{}", id, by.empty() ? "" : " by " + by);
    notify();
    return true;
}

std::size_t Alarms::acknowledge_all(const std::string& by) {
    std::size_t n = 0;
    {
        std::lock_guard lock(mutex_);
        const auto now = Alarm::Clock::now();
        for (auto& a : alarms_) {
            if (a.acknowledged) continue;
            a.acknowledged = true;
            a.acknowledged_at = now;
            a.acknowledged_by = by;
            ++n;
        }
        trim_history_locked();
    }
    if (n > 0) {
        log::info("{} alarm(s) acknowledged{}", n, by.empty() ? "" : " by " + by);
        notify();
    }
    return n;
}

void Alarms::purge_cleared() {
    {
        std::lock_guard lock(mutex_);
        alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(), [](const Alarm& a) { return !a.active; }),
                      alarms_.end());
    }
    notify();
}

void Alarms::trim_history_locked() {
    // Keep every active alarm; keep at most history_ cleared ones (newest).
    std::size_t cleared = 0;
    for (const auto& a : alarms_) cleared += a.active ? 0 : 1;
    while (cleared > history_) {
        auto oldest = alarms_.end();
        for (auto it = alarms_.begin(); it != alarms_.end(); ++it) {
            if (!it->active && (oldest == alarms_.end() || *it->cleared_at < *oldest->cleared_at)) oldest = it;
        }
        alarms_.erase(oldest);
        --cleared;
    }
}

std::vector<Alarm> Alarms::list() const {
    std::lock_guard lock(mutex_);
    auto out = alarms_;
    std::stable_sort(out.begin(), out.end(), [](const Alarm& a, const Alarm& b) {
        if (group_of(a) != group_of(b)) return group_of(a) < group_of(b);
        if (a.active && a.severity != b.severity) return a.severity > b.severity;
        const auto ta = a.active ? a.last_at : *a.cleared_at;
        const auto tb = b.active ? b.last_at : *b.cleared_at;
        return ta > tb;
    });
    return out;
}

std::optional<Alarm> Alarms::get(const std::string& id) const {
    std::lock_guard lock(mutex_);
    for (const auto& a : alarms_) {
        if (a.id == id) return a;
    }
    return std::nullopt;
}

bool Alarms::active(const std::string& id) const {
    auto a = get(id);
    return a && a->active;
}

std::size_t Alarms::active_count() const {
    std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(
        std::count_if(alarms_.begin(), alarms_.end(), [](const Alarm& a) { return a.active; }));
}

std::size_t Alarms::pending_count() const {
    std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(
        std::count_if(alarms_.begin(), alarms_.end(), [](const Alarm& a) { return a.pending(); }));
}

std::optional<Severity> Alarms::highest_active() const {
    std::lock_guard lock(mutex_);
    std::optional<Severity> out;
    for (const auto& a : alarms_) {
        if (a.active && (!out || a.severity > *out)) out = a.severity;
    }
    return out;
}

nlohmann::json Alarms::to_json() const {
    auto j = nlohmann::json::array();
    for (const auto& a : list()) j.push_back(forge::to_json(a));
    return j;
}

nlohmann::json Alarms::summary() const {
    const auto highest = highest_active();
    return {{"active", active_count()},
            {"pending", pending_count()},
            {"highest", highest ? nlohmann::json(to_string(*highest)) : nlohmann::json(nullptr)}};
}

Subscription Alarms::on_change(std::function<void()> listener) {
    return listeners_.add([l = std::move(listener)] {
        try {
            l();
        } catch (const std::exception& e) {
            log::error("alarm listener failed: {}", e.what());
        }
    });
}

void Alarms::notify() { listeners_.notify(); }

}  // namespace forge
