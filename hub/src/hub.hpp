#pragma once
// forge-hub: registry and dashboard of the forgecpp applications on a device.
//
//   applications --register/unregister--> [registry].listen (localhost)
//   hub --GET /health (or the "health" command)--> every registered application
//   hub --systemctl show--> units of the applications (state, restarts, policy)
//   browser --> http://<device>/        overview with links to every application
//
// Restarting is systemd's job: an application's unit (forge_add_app RESTART /
// MAX_RESTARTS / RESTART_WINDOW / WATCHDOG) restarts it after a crash or a hang
// and gives up after too many restarts. The hub shows what systemd does
// (restarts, "failed", why) with alarms, and starts / stops / restarts units on
// request. An application that is not the main process of a unit is only
// monitored: the hub reports its end but cannot restart it.

#include "proc.hpp"
#include "systemd.hpp"

#include <forge/forge.hpp>
#include <forge/hub.forge.hpp>
#include <forge/zmq.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#ifndef FORGE_HUB_VERSION
#define FORGE_HUB_VERSION "0.0.0"
#endif

namespace forgehub {

using forge::json;

struct Entry {
    forge_hub_app_registration app{};
    bool static_entry = false;
    std::string url, health_url;  ///< static entries

    std::string unit;             ///< systemd unit (verified: its main process registered)
    std::optional<UnitState> unit_state;
    std::uint32_t seen_restarts = 0;  ///< NRestarts already reported

    int pid = 0;
    std::uint64_t ticks = 0;      ///< process start time (pid reuse)
    forge_hub_app_state state = forge_hub_app_state_STATE_UNKNOWN;
    std::string message;
    bool stop_requested = false;  ///< stopped from the dashboard: not a crash

    // health (the application's /health)
    std::string health, alarms_highest;
    std::uint32_t alarms_active = 0;
    std::uint64_t uptime_s = 0;
    std::int64_t last_seen_ms = 0;   ///< unix ms
    std::int64_t failing_since = 0;  ///< steady ms of the first failed check (0 = ok)
    bool probing = false;
    std::int64_t next_probe = 0;

    // CPU / memory
    std::uint64_t cpu_ticks = 0;
    std::int64_t cpu_at = 0;
    double cpu = 0;
    std::uint64_t rss = 0;
};

/// What the dashboard shows (GET /api/apps).
struct AppList {
    std::string hub, boot_id;
    std::vector<forge_hub_app_status> apps;
};
json to_json(const AppList& l);

class Hub : public forge::Application {
public:
    explicit Hub(std::unique_ptr<Systemd> systemd = make_systemctl());

    /// Registration endpoint as bound (valid after start-up).
    std::string registry_endpoint() const;
    AppList list() const;
    std::optional<Entry> entry(const std::string& name) const;

    // Operator actions (also commands on the dashboard and forgectl call).
    json restart(const std::string& name);
    json stop_app(const std::string& name);
    json start(const std::string& name);
    json forget(const std::string& name);

protected:
    void setup_config(forge::config::Schema& schema) override;
    void setup_web(forge::web::Dashboard& ui) override;
    void setup_http(forge::http::Server& server) override;
    void setup_console(forge::Console& console) override;
    void on_start() override;
    void on_stop() override;

private:
    std::vector<std::string> handle(std::string_view request);  // registration thread
    void proxy_request(const forge::http::Request& req, forge::http::Response& res);  // HTTP threads
    forge::Status on_register(const forge_hub_app_registration& app, forge_hub_register_reply& reply);
    void on_unregister(const forge_hub_unregister_request& u);

    void tick();
    void apply_units(const std::map<std::string, UnitState>& units);
    void probe_done(const std::string& name, int pid, bool ok, const json& health, const std::string& error);
    void unit_action(const std::string& name, const std::vector<std::string>& verbs);

    // with mutex_ held
    void ended(Entry& e, const std::string& why, bool failure);
    forge_hub_app_status status_of(const Entry& e) const;
    void raise(const Entry& e, const std::string& kind, forge::Severity severity, const std::string& text);
    void clear(const Entry& e, const std::string& kind);
    std::string title_of(const Entry& e) const;

    void load_registry();
    void save_registry();

    std::unique_ptr<Systemd> systemd_;
    mutable std::mutex mutex_;
    std::map<std::string, Entry> apps_;
    bool dirty_ = false;
    bool querying_units_ = false;
    std::int64_t next_unit_query_ = 0;

    forge::tmtc::Registry registry_commands_;
    std::unique_ptr<forge::zmq::Server> registry_;
    forge::zmq::Stream* apps_stream_ = nullptr;
    std::string instance_;
    std::string boot_id_;

    // [registry]
    std::string state_file_;
    int tick_ms_ = 1000, probe_interval_ms_ = 2000, probe_timeout_ms_ = 1500, unresponsive_after_s_ = 5;
    bool use_systemd_ = true;
    std::atomic<int> proxy_timeout_ms_{10000};
};

}  // namespace forgehub
