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
    std::vector<Listener> listeners;
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
        listeners = listeners_;
    }
    const auto snap = snapshot();
    for (auto& l : listeners) l(snap);
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

v1::Health Health::snapshot() const {
    v1::Health h;
    h.set_state(static_cast<v1::ComponentHealth::State>(overall()));
    std::lock_guard lock(mutex_);
    for (const auto& [n, e] : components_) {
        auto* c = h.add_components();
        c->set_name(n);
        c->set_state(static_cast<v1::ComponentHealth::State>(e.state));
        c->set_message(e.message);
        c->set_critical(e.critical);
        c->set_since_unix_ms(
            std::chrono::duration_cast<std::chrono::milliseconds>(e.since.time_since_epoch()).count());
    }
    return h;
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

void Health::on_change(Listener listener) {
    std::lock_guard lock(mutex_);
    listeners_.push_back(std::move(listener));
}

}  // namespace forge
