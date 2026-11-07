#pragma once
// forge-hub: registry and supervisor of the forgecpp applications on a device.
//
//   applications --register/unregister--> [registry].listen (localhost)
//   hub --GET /health (or forge.v1 health)--> every registered application
//   browser --> http://<device>/        overview with links to every application
//
// A crash (the process ends without unregistering), a non-zero exit or a hang
// (health checks failing for hang_timeout_s) is handled with the restart
// policy the application declared: restart after delay_s, at most
// max_restarts within window_s, then give up (state FAILED, alarm). How it is
// restarted:
//   systemd  the process is the main process of a .service unit: systemctl
//            (if systemd already restarted it, the hub only counts it; on giving
//            up the unit is stopped so systemd's own restarts end too)
//   exec     otherwise: the same executable, arguments, directory, environment
//            and user, read by the hub from /proc when it registered

#include "proc.hpp"

#include <forge/forge.hpp>
#include <forge/v1/hub.pb.h>
#include <forge/zmq.hpp>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#ifndef FORGE_HUB_VERSION
#define FORGE_HUB_VERSION "0.0.0"
#endif

namespace forgehub {

namespace v1 = forge::v1;
using forge::json;

struct Entry {
    v1::AppRegistration app;
    bool static_entry = false;
    std::string url, health_url;  ///< static entries

    std::optional<LaunchInfo> launch;  ///< how to start it again (exec)
    std::string launch_error;          ///< why there is none
    std::string unit;                  ///< verified systemd unit ("" = exec)

    int pid = 0;
    std::uint64_t ticks = 0;  ///< process start time (pid reuse)
    int child = 0;            ///< started by the hub: reaped by it

    v1::AppStatus::State state = v1::AppStatus::STATE_UNKNOWN;
    std::string message;
    std::deque<std::int64_t> restarts;  ///< unix ms of restarts (window)
    std::uint32_t restarts_total = 0;

    // health (from the application's /health)
    std::string health, alarms_highest;
    std::uint32_t alarms_active = 0;
    std::uint64_t uptime_s = 0;
    std::int64_t last_seen_ms = 0;    ///< unix ms
    std::int64_t failing_since = 0;   ///< steady ms of the first failed check (0 = ok)
    std::string failing_why;
    bool probing = false;
    std::int64_t next_probe = 0;

    // CPU / memory
    std::uint64_t cpu_ticks = 0;
    std::int64_t cpu_at = 0;
    double cpu = 0;
    std::uint64_t rss = 0;

    // pending actions (steady ms, 0 = none)
    std::int64_t restart_at = 0;
    std::int64_t restart_wait_since = 0;
    std::int64_t start_deadline = 0;
    std::int64_t kill_at = 0;           ///< SIGKILL a process that ignores SIGTERM
    bool stop_requested = false;        ///< stopped on purpose: not a crash
    bool start_after_exit = false;      ///< restart requested: start when it has ended
    std::string pending_reason;         ///< why it ended, when the hub knows better (hung)
};

class Hub : public forge::Application {
public:
    Hub();

    /// Registration endpoint as bound (valid after start-up).
    std::string registry_endpoint() const;
    v1::AppList list() const;
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
    std::string handle(std::string_view request);  // registration thread
    v1::HubReply on_register(const v1::AppRegistration& app);
    void on_unregister(const v1::Unregister& u);

    void tick();
    void probe_done(const std::string& name, int pid, bool ok, const json& health, const std::string& error);

    // with mutex_ held
    void process_gone(Entry& e, const std::string& why, bool failure);
    void give_up(Entry& e, const std::string& why);
    void restart_now(Entry& e);
    void start_app(Entry& e, const std::string& why);
    void count_restart(Entry& e);
    std::uint32_t restarts_in_window(const Entry& e) const;
    v1::AppStatus status_of(const Entry& e) const;
    void raise(const Entry& e, forge::Severity severity, const std::string& text);
    void clear_alarm(const Entry& e);
    std::string title_of(const Entry& e) const;

    void systemctl(const std::string& name, std::vector<std::string> verbs, const std::string& unit);
    void load_registry();
    void save_registry();

    mutable std::mutex mutex_;
    std::map<std::string, Entry> apps_;
    bool dirty_ = false;

    std::unique_ptr<forge::zmq::Server> registry_;
    forge::zmq::Stream* apps_stream_ = nullptr;
    std::string instance_;
    std::string boot_id_;

    // [registry]
    std::string state_file_, log_dir_;
    int probe_interval_ms_ = 2000, probe_timeout_ms_ = 1500;
    int unresponsive_after_s_ = 5, start_timeout_s_ = 30;
    bool use_systemd_ = true;
};

}  // namespace forgehub
