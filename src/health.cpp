#include "forge/health.hpp"

#include <algorithm>

namespace forge {

const char* to_string(HealthState s) {
    switch (s) {
        case HealthState::ok: return "ok";
        case HealthState::degraded: return "degraded";
        case HealthState::failed: return "failed";
    }
    return "ok";
}

void Health::set(const std::string& component, HealthState state, const std::string& message, bool critical) {
    {
        std::lock_guard lock(mutex_);
        auto& e = components_[component];
        const bool changed = e.state != state || e.message != message || e.critical != critical ||
                             e.since == std::chrono::system_clock::time_point{};
        if (e.state != state || e.since == std::chrono::system_clock::time_point{}) {
            e.since = std::chrono::system_clock::now();
            e.since_steady = std::chrono::steady_clock::now();
        }
        e.state = state;
        e.message = message;
        e.critical = critical;
        if (!changed) return;
    }
    if (!listeners_.empty()) listeners_.notify(snapshot());
}

void Health::remove(const std::string& component) {
    std::lock_guard lock(mutex_);
    components_.erase(component);
}

HealthState Health::overall() const {
    std::lock_guard lock(mutex_);
    HealthState worst = HealthState::ok;
    for (const auto& [n, e] : components_) {
        // A failed non-critical component only degrades the service.
        const auto s = (e.state == HealthState::failed && !e.critical) ? HealthState::degraded : e.state;
        worst = std::max(worst, s);
    }
    return worst;
}

Health::Report Health::snapshot() const {
    Report r;
    r.state = overall();
    std::lock_guard lock(mutex_);
    for (const auto& [name, e] : components_) {
        r.components.push_back(
            {name, e.state, e.message, e.critical,
             std::chrono::duration_cast<std::chrono::milliseconds>(e.since.time_since_epoch()).count()});
    }
    return r;
}

std::optional<double> Health::critical_failed_for() const {
    std::lock_guard lock(mutex_);
    std::optional<double> worst;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [n, e] : components_) {
        if (!e.critical || e.state != HealthState::failed) continue;
        const double s = std::chrono::duration<double>(now - e.since_steady).count();
        if (!worst || s > *worst) worst = s;
    }
    return worst;
}

json to_json(const Health::Report& r) {
    json components = json::array();
    for (const auto& c : r.components) {
        components.push_back({{"name", c.name},
                              {"state", to_string(c.state)},
                              {"message", c.message},
                              {"critical", c.critical},
                              {"since_unix_ms", c.since_unix_ms}});
    }
    return {{"state", to_string(r.state)}, {"components", std::move(components)}};
}

}  // namespace forge
