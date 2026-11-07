#include "hub.hpp"

#include <forge/hub.hpp>
#include <forge/proto.hpp>

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <random>

using namespace std::chrono_literals;

namespace forgehub {
namespace {

std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool live(v1::AppStatus::State s) {
    return s == v1::AppStatus::STATE_RUNNING || s == v1::AppStatus::STATE_UNRESPONSIVE ||
           s == v1::AppStatus::STATE_STARTING;
}

bool valid_name(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    return std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.'; });
}

/// Address to reach a listening endpoint from this machine ("tcp://*:5557" -> tcp://127.0.0.1:5557).
std::string local_endpoint(std::string ep) {
    for (const std::string any : {"tcp://*:", "tcp://0.0.0.0:", "tcp://[::]:"}) {
        if (ep.rfind(any, 0) == 0) return "tcp://127.0.0.1:" + ep.substr(any.size());
    }
    return ep;
}

std::string seconds(std::uint64_t us) {
    const auto s = us / 1000000.0;
    return s == static_cast<std::uint64_t>(s) ? std::to_string(static_cast<std::uint64_t>(s)) : fmt::format("{:.1f}", s);
}

/// The restart policy of a unit as forge.v1 describes it.
v1::RestartPolicy policy_of(const UnitState& u) {
    v1::RestartPolicy p;
    p.set_restart(u.restart);
    p.set_mode(u.restart == "no" || u.restart.empty() ? v1::RestartPolicy::MODE_NEVER
               : u.restart == "always"                 ? v1::RestartPolicy::MODE_ALWAYS
                                                       : v1::RestartPolicy::MODE_ON_FAILURE);
    if (u.start_limit_interval_us > 0 && u.start_limit_burst > 0) {
        p.set_max_restarts(u.start_limit_burst - 1);  // the first start counts too
        p.set_window_s(static_cast<std::uint32_t>(u.start_limit_interval_us / 1000000));
    }
    p.set_delay_s(static_cast<std::uint32_t>(u.restart_us / 1000000));
    p.set_watchdog_s(static_cast<std::uint32_t>(u.watchdog_us / 1000000));
    return p;
}

/// Why a unit stopped, in words (systemd's Result=).
std::string result_text(const std::string& result) {
    if (result == "exit-code") return "exited with an error code";
    if (result == "signal") return "killed by a signal";
    if (result == "core-dump") return "crashed (core dump)";
    if (result == "watchdog") return "hung (watchdog timeout)";
    if (result == "timeout") return "timed out";
    if (result == "start-limit-hit") return "restarted too often";
    if (result == "oom-kill") return "killed: out of memory";
    if (result == "resources") return "could not start (resources)";
    return result;
}

}  // namespace

Hub::Hub(std::unique_ptr<Systemd> systemd)
    : Application({"forge-hub", FORGE_HUB_VERSION, "Applications on this device"}), systemd_(std::move(systemd)) {
    disable_hub_registration();  // the hub does not register with itself
    std::random_device rd;
    instance_ = fmt::format("{:08x}{:08x}", rd(), rd());
}

// ---------------------------------------------------------------------------
// configuration

void Hub::setup_config(forge::config::Schema& s) {
    s.section("registry", "Application registry");
    s.string("registry.listen", forge::hub::default_endpoint,
             "Registration endpoint (keep it on localhost: it accepts any local process)");
    s.string("registry.state_file", "", "Remember the applications across hub restarts (JSON)");
    s.integer("registry.tick_ms", 1000, "How often processes are checked").range(20, 60000).unit("ms");
    s.integer("registry.probe_interval_ms", 2000, "Health check and systemd query interval").range(50, 600000).unit("ms");
    s.integer("registry.probe_timeout_ms", 1500, "Health check timeout").range(50, 60000).unit("ms");
    s.integer("registry.unresponsive_after_s", 5, "Failing health checks this long: unresponsive").range(0, 3600).unit("s");
    s.boolean("registry.systemd", true, "Follow the systemd units of the applications (state, restarts, actions)");
    s.string("registry.boot_id_file", "/proc/sys/kernel/random/boot_id", "Boot id (processes of an earlier boot are gone)");
    s.open_section("static", "[static.<name>] url, health_url, title, description, group: applications that do not register");
}

void Hub::setup_web(forge::web::Dashboard& ui) {
    ui.title("Device");
    ui.link("Applications", "/");
    auto& g = ui.group("Applications");
    auto count = [this](auto pred) {
        std::lock_guard lock(mutex_);
        return static_cast<int>(std::count_if(apps_.begin(), apps_.end(), [&](const auto& kv) { return pred(kv.second); }));
    };
    g.value("apps", "Known", [count] { return count([](const Entry&) { return true; }); });
    g.value("running", "Running", [count] {
        return count([](const Entry& e) { return e.state == v1::AppStatus::STATE_RUNNING; });
    });
    g.value("failed", "Failed / crashed", [count] {
        return count([](const Entry& e) {
            return e.state == v1::AppStatus::STATE_FAILED || e.state == v1::AppStatus::STATE_CRASHED;
        });
    });
    g.value("registry", "Registration endpoint", [this] { return registry_endpoint(); });
    using forge::web::text_param;
    g.command("restart", "Restart", {text_param("name", std::nullopt, "application")},
              [this](const json& a) { return restart(a["name"].get<std::string>()); })
        .help("systemctl restart (also after the unit failed)");
    g.command("stop", "Stop", {text_param("name", std::nullopt, "application")},
              [this](const json& a) { return stop_app(a["name"].get<std::string>()); })
        .help("systemctl stop (SIGTERM for applications without a unit)");
    g.command("start", "Start", {text_param("name", std::nullopt, "application")},
              [this](const json& a) { return start(a["name"].get<std::string>()); })
        .help("systemctl start (also after the unit failed)");
    g.command("forget", "Forget", {text_param("name", std::nullopt, "application")},
              [this](const json& a) { return forget(a["name"].get<std::string>()); })
        .help("Remove an application that is not running from the list");
}

void Hub::setup_http(forge::http::Server& server) {
    server.get_json("/api/apps", [this](const forge::http::Request&) { return forge::proto::to_json(list()); });
}

void Hub::setup_console(forge::Console& c) {
    c.add("apps", "apps  applications on this device", [this, &c](const forge::Console::Args&) {
        const auto l = list();
        if (l.apps().empty()) c.print("no applications registered");
        for (const auto& a : l.apps()) {
            const auto state = v1::AppStatus::State_Name(a.state()).substr(6);
            c.print("  {:<22} {:<12} pid {:<7} http {:<5} restarts {:<3} {:<28} {}", a.app().name(), state,
                    a.app().pid(), a.app().http_port(), a.restarts(), a.supervision(), a.message());
        }
    });
}

// ---------------------------------------------------------------------------
// lifecycle

void Hub::on_start() {
    const auto cfg = config();
    state_file_ = cfg.value<std::string>("registry.state_file");
    tick_ms_ = cfg.value<int>("registry.tick_ms");
    probe_interval_ms_ = cfg.value<int>("registry.probe_interval_ms");
    probe_timeout_ms_ = cfg.value<int>("registry.probe_timeout_ms");
    unresponsive_after_s_ = cfg.value<int>("registry.unresponsive_after_s");
    use_systemd_ = cfg.value<bool>("registry.systemd");
    boot_id_ = boot_id(cfg.value<std::string>("registry.boot_id_file"));

    {
        std::lock_guard lock(mutex_);
        // Static entries: applications that do not register (other web interfaces).
        const auto root = cfg.to_json();
        if (root.contains("static") && root["static"].is_object()) {
            for (const auto& [name, s] : root["static"].items()) {
                if (!s.is_object()) continue;
                Entry e;
                e.static_entry = true;
                e.app.set_name(name);
                e.app.set_title(s.value("title", name));
                e.app.set_description(s.value("description", ""));
                e.app.set_group(s.value("group", ""));
                e.url = s.value("url", "");
                e.health_url = s.value("health_url", "");
                e.state = e.health_url.empty() ? v1::AppStatus::STATE_UNKNOWN : v1::AppStatus::STATE_STARTING;
                e.message = e.health_url.empty() ? "no health check" : "";
                apps_[name] = std::move(e);
            }
        }
    }
    load_registry();

    registry_ = std::make_unique<forge::zmq::Server>(
        cfg.value<std::string>("registry.listen"),
        [this](std::string_view req) { return handle(req); });
    logger()->info("registry on {}", registry_->endpoint());
    apps_stream_ = &stream("apps", "forge.v1.AppList", {"latest", 2});
    every(std::chrono::milliseconds(tick_ms_), [this] { tick(); }, true);
}

void Hub::on_stop() {
    if (registry_) registry_->stop();
    std::lock_guard lock(mutex_);
    dirty_ = true;
    save_registry();
}

std::string Hub::registry_endpoint() const { return registry_ ? registry_->endpoint() : std::string(); }

// ---------------------------------------------------------------------------
// registration (registry thread)

std::string Hub::handle(std::string_view bytes) {
    v1::HubRequest req;
    v1::HubReply reply;
    reply.set_hub(std::string("forge-hub ") + FORGE_HUB_VERSION);
    reply.set_instance(instance_);
    if (!req.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        reply.set_error("not a forge.v1.HubRequest");
    } else if (req.has_register_()) {
        reply = on_register(req.register_());
    } else if (req.has_unregister()) {
        on_unregister(req.unregister());
        reply.set_ok(true);
    } else {
        reply.set_error("empty request");
    }
    return reply.SerializeAsString();
}

v1::HubReply Hub::on_register(const v1::AppRegistration& app) {
    v1::HubReply reply;
    reply.set_hub(std::string("forge-hub ") + FORGE_HUB_VERSION);
    reply.set_instance(instance_);
    const auto& name = app.name();
    const int pid = static_cast<int>(app.pid());
    if (!valid_name(name) || pid <= 0) {
        reply.set_error("invalid name or pid");
        return reply;
    }
    const auto ticks = start_ticks(pid);
    if (ticks == 0) {
        reply.set_error("no process " + std::to_string(pid) + " on this machine (the hub only lists local applications)");
        return reply;
    }
    // systemctl before taking the lock (slow).
    const auto unit = use_systemd_ ? systemd_->unit_of(pid) : std::string();

    std::lock_guard lock(mutex_);
    auto it = apps_.find(name);
    if (it != apps_.end() && !it->second.static_entry && it->second.pid > 0 && it->second.pid != pid &&
        live(it->second.state) && alive(it->second.pid, it->second.ticks)) {
        reply.set_error(name + " is already registered by pid " + std::to_string(it->second.pid));
        return reply;
    }
    Entry& e = apps_[name];
    if (e.static_entry) {
        reply.set_error(name + " is a static entry of the hub's configuration");
        return reply;
    }
    // The same process registering again (retry_s heartbeat) changes nothing.
    const bool same = e.pid == pid && e.ticks == ticks &&
                      (e.state == v1::AppStatus::STATE_RUNNING || e.state == v1::AppStatus::STATE_UNRESPONSIVE);
    if (!same) {
        e.pid = pid;
        e.ticks = ticks;
        e.failing_since = 0;
        e.next_probe = 0;
        e.cpu_at = 0;
        e.stop_requested = false;
        e.state = v1::AppStatus::STATE_RUNNING;
        e.message.clear();
        clear(e, "ended");
        clear(e, "failed");
        logger()->info("registered: {} {} (pid {}{}{})", name, app.version(), pid,
                       app.http_port() ? ", http " + std::to_string(app.http_port()) : std::string(),
                       unit.empty() ? std::string() : ", " + unit);
    }
    e.app = app;
    e.unit = unit;
    e.last_seen_ms = unix_ms();
    if (!unit.empty()) next_unit_query_ = 0;  // pick up its policy right away
    dirty_ = true;
    reply.set_ok(true);
    return reply;
}

void Hub::on_unregister(const v1::Unregister& u) {
    std::lock_guard lock(mutex_);
    auto it = apps_.find(u.name());
    if (it == apps_.end() || it->second.static_entry) return;
    Entry& e = it->second;
    if (e.pid != static_cast<int>(u.pid()) || !live(e.state)) return;
    if (u.reason() == v1::Unregister::REASON_SIGNAL || e.stop_requested) {
        ended(e, "stopped", false);
    } else if (u.exit_code() == 0) {
        ended(e, "exited", false);
    } else {
        ended(e, "exited with code " + std::to_string(u.exit_code()), true);
    }
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// monitoring (main loop)

std::string Hub::title_of(const Entry& e) const { return e.app.title().empty() ? e.app.name() : e.app.title(); }

void Hub::raise(const Entry& e, const std::string& kind, forge::Severity severity, const std::string& text) {
    alarms().raise("app." + e.app.name() + "." + kind, severity, title_of(e) + ": " + text);
}

void Hub::clear(const Entry& e, const std::string& kind) { alarms().clear("app." + e.app.name() + "." + kind); }

void Hub::ended(Entry& e, const std::string& why, bool failure) {
    e.pid = 0;
    e.cpu = 0;
    e.rss = 0;
    e.uptime_s = 0;
    e.failing_since = 0;
    e.stop_requested = false;
    e.state = failure ? v1::AppStatus::STATE_CRASHED : v1::AppStatus::STATE_STOPPED;
    e.message = why;
    if (!e.unit.empty()) {
        // systemd decides what happens next (restart or not); the next query shows it.
        next_unit_query_ = 0;
    } else if (failure) {
        e.message += " (not a systemd unit: not restarted)";
    }
    if (failure) {
        logger()->warn("{}: {}", e.app.name(), why);
        raise(e, "ended", forge::Severity::warning, why);
    } else {
        logger()->info("{}: {}", e.app.name(), why);
    }
}

void Hub::apply_units(const std::map<std::string, UnitState>& units) {
    std::lock_guard lock(mutex_);
    querying_units_ = false;
    for (auto& [name, e] : apps_) {
        if (e.unit.empty()) continue;
        const auto it = units.find(e.unit);
        if (it == units.end() || !it->second.loaded) {
            e.unit_state.reset();
            continue;
        }
        const auto& u = it->second;
        e.unit_state = u;
        if (u.n_restarts > e.seen_restarts) {
            raise(e, "ended", forge::Severity::warning,
                  fmt::format("{}; restarted by systemd ({} time{})", result_text(u.result), u.n_restarts,
                              u.n_restarts == 1 ? "" : "s"));
        }
        e.seen_restarts = u.n_restarts;

        const auto policy = policy_of(u);
        if (u.active == "failed") {
            if (e.state != v1::AppStatus::STATE_FAILED) {
                e.state = v1::AppStatus::STATE_FAILED;
                // An automatic restart refused by the start limit leaves the last
                // run's Result (e.g. "signal"); a manual start refused says start-limit-hit.
                const bool gave_up = u.result == "start-limit-hit" ||
                                     (policy.max_restarts() > 0 && u.n_restarts >= policy.max_restarts());
                e.message = gave_up ? fmt::format("gave up after {} restarts in {} s (last: {})", u.n_restarts,
                                                  policy.window_s(), result_text(u.result))
                                    : "failed: " + result_text(u.result);
                raise(e, "failed", e.app.critical() ? forge::Severity::critical : forge::Severity::error, e.message);
                logger()->error("{}: {}", name, e.message);
                dirty_ = true;
            }
            e.pid = 0;
            continue;
        }
        clear(e, "failed");
        if (u.active == "activating") {
            e.state = v1::AppStatus::STATE_STARTING;
            e.message = u.sub == "auto-restart"
                            ? fmt::format("{}; systemd restarts it in {} s", result_text(u.result), seconds(u.restart_us))
                            : "starting";
            e.pid = 0;
        } else if (u.active == "active") {
            if (u.main_pid > 0 && u.main_pid == e.pid) {
                if (e.state != v1::AppStatus::STATE_UNRESPONSIVE) e.state = v1::AppStatus::STATE_RUNNING;
            } else if (u.main_pid > 0) {
                // Started (again) by systemd; it registers in a moment.
                e.state = v1::AppStatus::STATE_STARTING;
                e.message = fmt::format("started by systemd (pid {}), waiting for it to register", u.main_pid);
            }
        } else if (u.active == "inactive" || u.active == "deactivating") {
            if (e.state != v1::AppStatus::STATE_STOPPED) {
                e.state = v1::AppStatus::STATE_STOPPED;
                e.message = u.active == "deactivating" ? "stopping" : "stopped";
                dirty_ = true;
            }
            e.pid = 0;
        }
    }
}

void Hub::tick() {
    struct Probe {
        std::string name;
        int pid;
        std::string url, control;
    };
    std::vector<Probe> probes;
    std::vector<std::string> units;
    const auto now = steady_ms();
    {
        std::lock_guard lock(mutex_);
        for (auto& [name, e] : apps_) {
            if (e.static_entry) {
                if (!e.health_url.empty() && !e.probing && now >= e.next_probe) {
                    e.probing = true;
                    probes.push_back({name, 0, e.health_url, {}});
                }
                continue;
            }
            if (!e.unit.empty()) units.push_back(e.unit);
            if (live(e.state) && e.pid > 0 && !alive(e.pid, e.ticks)) {
                ended(e, "ended without unregistering", true);
                dirty_ = true;
            }
            if (!live(e.state) || e.pid <= 0) continue;

            if (const auto u = usage(e.pid)) {
                if (e.cpu_at > 0 && u->cpu_ticks >= e.cpu_ticks && now > e.cpu_at) {
                    e.cpu = 100.0 * static_cast<double>(u->cpu_ticks - e.cpu_ticks) / ticks_per_second() /
                            ((now - e.cpu_at) / 1000.0);
                }
                e.cpu_ticks = u->cpu_ticks;
                e.cpu_at = now;
                e.rss = u->rss_bytes;
            }
            if (!e.probing && now >= e.next_probe && e.state != v1::AppStatus::STATE_STARTING) {
                std::string control;
                const auto it = e.app.endpoints().find("control");
                if (it != e.app.endpoints().end()) control = local_endpoint(it->second);
                if (e.app.http_port() > 0 || !control.empty()) {
                    e.probing = true;
                    probes.push_back({name, e.pid,
                                      e.app.http_port() > 0
                                          ? fmt::format("http://127.0.0.1:{}/health", e.app.http_port())
                                          : std::string(),
                                      control});
                }
            }
        }
        if (units.empty() || querying_units_ || now < next_unit_query_) {
            units.clear();
        } else {
            querying_units_ = true;
            next_unit_query_ = now + probe_interval_ms_;
        }
        if (dirty_) save_registry();
    }

    if (!units.empty()) {
        pool().detach_task([this, units] {
            auto states = systemd_->show(units);
            post([this, states = std::move(states)] { apply_units(states); });
        });
    }
    const auto timeout = std::chrono::milliseconds(probe_timeout_ms_);
    for (auto& p : probes) {
        pool().detach_task([this, p, timeout] {
            json health;
            std::string error;
            bool ok = false;
            try {
                if (!p.url.empty()) {
                    const auto scheme_end = p.url.find("://");
                    const auto path_start = p.url.find('/', scheme_end == std::string::npos ? 0 : scheme_end + 3);
                    forge::http::Client c(p.url.substr(0, path_start));
                    c.timeout(timeout);
                    health = c.get_json(path_start == std::string::npos ? std::string("/") : p.url.substr(path_start));
                    ok = true;
                } else {
                    forge::zmq::Client c(p.control, timeout);
                    c.set_client("forge-hub");
                    v1::Request req;
                    req.mutable_health();
                    const auto h = c.call(req).health();
                    v1::Request areq;
                    areq.mutable_alarms();
                    int active = 0;
                    const auto alarms = c.call(areq);  // keep the reply alive while iterating
                    for (const auto& a : alarms.alarms().alarms()) active += a.active() ? 1 : 0;
                    health = {{"status", "ok"},
                              {"health", forge::to_string(static_cast<forge::HealthState>(h.state() ? h.state() : 1))},
                              {"alarms", {{"active", active}}}};
                    ok = true;
                }
            } catch (const std::exception& e) {
                error = e.what();
            }
            post([this, p, ok, health, error] { probe_done(p.name, p.pid, ok, health, error); });
        });
    }

    if (apps_stream_ && apps_stream_->wanted()) apps_stream_->publish(list());
}

void Hub::probe_done(const std::string& name, int pid, bool ok, const json& health, const std::string& error) {
    std::lock_guard lock(mutex_);
    auto it = apps_.find(name);
    if (it == apps_.end()) return;
    Entry& e = it->second;
    e.probing = false;
    const auto now = steady_ms();
    e.next_probe = now + probe_interval_ms_;
    if (!e.static_entry && (e.pid != pid || !live(e.state))) return;  // it changed meanwhile

    std::string why;
    if (ok) {
        if (!e.static_entry && health.contains("pid") && health["pid"].is_number() &&
            health["pid"].get<int>() != e.pid) {
            why = fmt::format("its web port is answered by pid {}", health["pid"].get<int>());
        } else if (health.value("status", "ok") == "stalled") {
            why = "main loop stalled";
        }
        e.health = health.value("health", e.static_entry ? "ok" : "");
        e.uptime_s = health.value("uptime_s", std::uint64_t{0});
        if (health.contains("alarms") && health["alarms"].is_object()) {
            const auto& a = health["alarms"];
            e.alarms_active = a.value("active", 0u);
            e.alarms_highest = a.contains("highest") && a["highest"].is_string() ? a["highest"].get<std::string>() : "";
        }
    } else {
        why = error.empty() ? "no answer" : error;
    }

    if (why.empty()) {
        e.last_seen_ms = unix_ms();
        e.failing_since = 0;
        if (e.state == v1::AppStatus::STATE_UNRESPONSIVE || (e.static_entry && e.state != v1::AppStatus::STATE_RUNNING)) {
            e.state = v1::AppStatus::STATE_RUNNING;
            e.message.clear();
            clear(e, "unresponsive");
        }
        return;
    }
    if (!e.failing_since) e.failing_since = now;
    if (now - e.failing_since >= unresponsive_after_s_ * 1000LL && e.state != v1::AppStatus::STATE_UNRESPONSIVE) {
        e.state = v1::AppStatus::STATE_UNRESPONSIVE;
        e.message = why + (e.unit_state && e.unit_state->watchdog_us
                               ? fmt::format(" (systemd's watchdog restarts a hung main loop after {} s)",
                                             seconds(e.unit_state->watchdog_us))
                               : std::string());
        logger()->warn("{}: unresponsive: {}", name, why);
        raise(e, "unresponsive", forge::Severity::warning, "unresponsive: " + why);
    }
}

// ---------------------------------------------------------------------------
// status and actions

v1::AppStatus Hub::status_of(const Entry& e) const {
    v1::AppStatus s;
    *s.mutable_app() = e.app;
    if (!e.static_entry) s.mutable_app()->set_pid(live(e.state) ? static_cast<std::uint32_t>(std::max(e.pid, 0)) : 0);
    s.set_state(e.state);
    s.set_message(e.message);
    s.set_health(e.health);
    s.set_alarms_active(e.alarms_active);
    s.set_alarms_highest(e.alarms_highest);
    s.set_uptime_s(live(e.state) ? e.uptime_s : 0);
    s.set_cpu_percent(e.cpu);
    s.set_rss_bytes(e.rss);
    s.set_supervision(e.unit.empty() ? "none" : "systemd " + e.unit);
    s.set_last_seen_unix_ms(e.last_seen_ms);
    s.set_url(e.url);
    s.set_static_entry(e.static_entry);
    if (e.unit_state) {
        s.set_restarts(e.unit_state->n_restarts);
        *s.mutable_policy() = policy_of(*e.unit_state);
        s.set_unit_state(e.unit_state->active + "/" + e.unit_state->sub);
        s.set_unit_result(e.unit_state->result);
    }
    return s;
}

v1::AppList Hub::list() const {
    v1::AppList l;
    l.set_hub(std::string("forge-hub ") + FORGE_HUB_VERSION);
    l.set_boot_id(boot_id_);
    std::lock_guard lock(mutex_);
    for (const auto& [name, e] : apps_) *l.add_apps() = status_of(e);
    return l;
}

std::optional<Entry> Hub::entry(const std::string& name) const {
    std::lock_guard lock(mutex_);
    auto it = apps_.find(name);
    if (it == apps_.end()) return std::nullopt;
    return it->second;
}

namespace {
Entry& find_or_throw(std::map<std::string, Entry>& apps, const std::string& name) {
    auto it = apps.find(name);
    if (it == apps.end()) throw forge::Error(404, "unknown application '" + name + "'");
    if (it->second.static_entry) {
        throw forge::Error(409, "'" + name + "' is a static entry: the hub does not manage it");
    }
    return it->second;
}
}  // namespace

void Hub::unit_action(const std::string& name, std::vector<std::string> verbs) {
    std::string unit;
    {
        std::lock_guard lock(mutex_);
        const auto it = apps_.find(name);
        if (it == apps_.end() || it->second.unit.empty()) return;
        unit = it->second.unit;
    }
    // systemctl can take seconds: off the main loop.
    pool().detach_task([this, name, unit, verbs] {
        std::string error;
        for (const auto& verb : verbs) {
            if (!systemd_->action(verb, unit, &error) && verb != "reset-failed") break;
            error.clear();
        }
        post([this, name, error] {
            std::lock_guard lock(mutex_);
            next_unit_query_ = 0;
            if (error.empty()) return;
            logger()->error("{}", error);
            auto it = apps_.find(name);
            if (it != apps_.end()) it->second.message = error;
        });
    });
}

json Hub::restart(const std::string& name) {
    {
        std::lock_guard lock(mutex_);
        Entry& e = find_or_throw(apps_, name);
        if (e.unit.empty()) {
            throw forge::Error(409, "'" + name + "' is not a systemd unit: the hub cannot restart it");
        }
        e.state = v1::AppStatus::STATE_STARTING;
        e.message = "restart requested";
    }
    unit_action(name, {"reset-failed", "restart"});
    return {{"name", name}, {"action", "restart"}};
}

json Hub::stop_app(const std::string& name) {
    bool unit = false;
    {
        std::lock_guard lock(mutex_);
        Entry& e = find_or_throw(apps_, name);
        unit = !e.unit.empty();
        if (!unit && (!live(e.state) || e.pid <= 0 || !alive(e.pid, e.ticks))) {
            throw forge::Error(409, "'" + name + "' is not running");
        }
        e.stop_requested = true;  // its unregistration is then a stop, not a crash
        e.message = "stopping";
        if (!unit) ::kill(e.pid, SIGTERM);
    }
    if (unit) unit_action(name, {"stop"});
    return {{"name", name}, {"action", "stop"}};
}

json Hub::start(const std::string& name) {
    {
        std::lock_guard lock(mutex_);
        Entry& e = find_or_throw(apps_, name);
        if (e.unit.empty()) {
            throw forge::Error(409, "'" + name + "' is not a systemd unit: the hub cannot start it");
        }
        if (live(e.state) && e.pid > 0 && alive(e.pid, e.ticks)) {
            throw forge::Error(409, "'" + name + "' is already running");
        }
        e.state = v1::AppStatus::STATE_STARTING;
        e.message = "start requested";
    }
    unit_action(name, {"reset-failed", "start"});
    return {{"name", name}, {"action", "start"}};
}

json Hub::forget(const std::string& name) {
    std::lock_guard lock(mutex_);
    Entry& e = find_or_throw(apps_, name);
    if (live(e.state) && e.pid > 0 && alive(e.pid, e.ticks)) {
        throw forge::Error(409, "'" + name + "' is running; stop it first");
    }
    for (const auto* kind : {"ended", "failed", "unresponsive"}) clear(e, kind);
    apps_.erase(name);
    dirty_ = true;
    return {{"name", name}, {"forgotten", true}};
}

// ---------------------------------------------------------------------------
// persistence

void Hub::save_registry() {
    dirty_ = false;
    if (state_file_.empty()) return;
    json apps = json::array();
    for (const auto& [name, e] : apps_) {
        if (e.static_entry) continue;
        apps.push_back({{"app", forge::proto::to_json(e.app)},
                        {"unit", e.unit},
                        {"pid", e.pid},
                        {"ticks", e.ticks},
                        {"state", static_cast<int>(e.state)},
                        {"message", e.message}});
    }
    try {
        forge::save_json(state_file_, json{{"boot_id", boot_id_}, {"apps", std::move(apps)}});
    } catch (const std::exception& e) {
        logger()->warn("cannot save {}: {}", state_file_, e.what());
    }
}

void Hub::load_registry() {
    if (state_file_.empty() || !std::filesystem::exists(state_file_)) return;
    json doc;
    try {
        doc = forge::load_json(state_file_);
    } catch (const std::exception& e) {
        logger()->warn("ignoring {}: {}", state_file_, e.what());
        return;
    }
    const bool same_boot = !boot_id_.empty() && doc.value("boot_id", "") == boot_id_;
    std::lock_guard lock(mutex_);
    for (const auto& j : doc.value("apps", json::array())) {
        Entry e;
        try {
            forge::proto::from_json_string(j.at("app").dump(), &e.app);
        } catch (const std::exception&) {
            continue;
        }
        if (!valid_name(e.app.name()) || apps_.count(e.app.name())) continue;
        e.unit = use_systemd_ ? j.value("unit", "") : std::string();
        e.pid = j.value("pid", 0);
        e.ticks = j.value("ticks", std::uint64_t{0});
        const auto state = static_cast<v1::AppStatus::State>(j.value("state", 0));
        if (same_boot && live(state) && alive(e.pid, e.ticks)) {
            e.state = v1::AppStatus::STATE_RUNNING;  // checked again right away
        } else {
            // Units: systemd says what they are doing at the first query.
            e.pid = 0;
            e.state = same_boot && !live(state) ? state : v1::AppStatus::STATE_UNKNOWN;
            e.message = e.state == v1::AppStatus::STATE_UNKNOWN ? "not running" : j.value("message", "");
        }
        apps_[e.app.name()] = std::move(e);
    }
    logger()->info("registry: {} application(s) from {}", apps_.size(), state_file_);
}

}  // namespace forgehub
