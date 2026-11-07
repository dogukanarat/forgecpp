#include "hub.hpp"

#include <forge/hub.hpp>
#include <forge/proto.hpp>

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
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

std::string reply_bytes(bool ok, const std::string& error, const std::string& instance) {
    v1::HubReply r;
    r.set_ok(ok);
    r.set_error(error);
    r.set_hub(std::string("forge-hub ") + FORGE_HUB_VERSION);
    r.set_instance(instance);
    return r.SerializeAsString();
}

/// Address to reach a listening endpoint from this machine ("tcp://*:5557" -> tcp://127.0.0.1:5557).
std::string local_endpoint(std::string ep) {
    for (const std::string any : {"tcp://*:", "tcp://0.0.0.0:", "tcp://[::]:"}) {
        if (ep.rfind(any, 0) == 0) return "tcp://127.0.0.1:" + ep.substr(any.size());
    }
    return ep;
}

std::string describe_exit(int status) {
    if (WIFSIGNALED(status)) return std::string("killed by ") + ::strsignal(WTERMSIG(status));
    if (WIFEXITED(status)) return "exited with code " + std::to_string(WEXITSTATUS(status));
    return "ended";
}

json launch_json(const LaunchInfo& l) {
    return {{"exe", l.exe}, {"argv", l.argv}, {"cwd", l.cwd}, {"env", l.env}, {"uid", l.uid}, {"gid", l.gid}};
}

LaunchInfo launch_from(const json& j) {
    LaunchInfo l;
    l.exe = j.value("exe", "");
    l.argv = j.value("argv", std::vector<std::string>{});
    l.cwd = j.value("cwd", "/");
    l.env = j.value("env", std::vector<std::string>{});
    l.uid = j.value("uid", 0u);
    l.gid = j.value("gid", 0u);
    return l;
}

}  // namespace

Hub::Hub() : Application({"forge-hub", FORGE_HUB_VERSION, "Applications on this device"}) {
    disable_hub_registration();  // the hub does not register with itself
    std::random_device rd;
    instance_ = fmt::format("{:08x}{:08x}", rd(), rd());
}

// ---------------------------------------------------------------------------
// configuration

void Hub::setup_config(forge::config::Schema& s) {
    s.section("registry", "Application registry and supervision");
    s.string("registry.listen", forge::hub::default_endpoint,
             "Registration endpoint (keep it on localhost: it accepts any local process)");
    s.string("registry.state_file", "", "Remember the applications across hub restarts (JSON, mode 0600)");
    s.string("registry.log_dir", "", "Output of applications the hub starts itself (<name>.log; empty = discarded)");
    s.integer("registry.probe_interval_ms", 2000, "Health check interval").range(200, 600000).unit("ms");
    s.integer("registry.probe_timeout_ms", 1500, "Health check timeout").range(100, 60000).unit("ms");
    s.integer("registry.unresponsive_after_s", 5, "Failing health checks this long: unresponsive").range(1, 3600).unit("s");
    s.integer("registry.start_timeout_s", 30, "A started application must register within this time").range(1, 3600).unit("s");
    s.boolean("registry.systemd", true, "Restart applications that are systemd units with systemctl");
    s.string("registry.boot_id_file", "/proc/sys/kernel/random/boot_id", "Boot id (entries from an earlier boot are not restarted)");
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
        .help("Restart an application (resets its restart count)");
    g.command("stop", "Stop", {text_param("name", std::nullopt, "application")},
              [this](const json& a) { return stop_app(a["name"].get<std::string>()); })
        .help("Stop an application; it is not restarted until started again");
    g.command("start", "Start", {text_param("name", std::nullopt, "application")},
              [this](const json& a) { return start(a["name"].get<std::string>()); })
        .help("Start a stopped or failed application");
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
            c.print("  {:<22} {:<12} pid {:<7} http {:<5} restarts {:<3} {}", a.app().name(), state, a.app().pid(),
                    a.app().http_port(), a.restarts(), a.message());
        }
    });
}

// ---------------------------------------------------------------------------
// lifecycle

void Hub::on_start() {
    const auto cfg = config();
    state_file_ = cfg.get_or<std::string>("registry.state_file", "");
    log_dir_ = cfg.get_or<std::string>("registry.log_dir", "");
    probe_interval_ms_ = cfg.get_or<int>("registry.probe_interval_ms", 2000);
    probe_timeout_ms_ = cfg.get_or<int>("registry.probe_timeout_ms", 1500);
    unresponsive_after_s_ = cfg.get_or<int>("registry.unresponsive_after_s", 5);
    start_timeout_s_ = cfg.get_or<int>("registry.start_timeout_s", 30);
    use_systemd_ = cfg.get_or<bool>("registry.systemd", true);
    boot_id_ = boot_id(cfg.get_or<std::string>("registry.boot_id_file", "/proc/sys/kernel/random/boot_id"));
    if (!log_dir_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(log_dir_, ec);
    }

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
        cfg.get_or<std::string>("registry.listen", forge::hub::default_endpoint),
        [this](std::string_view req) { return handle(req); });
    logger()->info("registry on {}{}", registry_->endpoint(),
                   ::geteuid() == 0 ? "" : " (not root: applications of other users cannot be restarted)");
    apps_stream_ = &stream("apps", "forge.v1.AppList", {"latest", 2});
    every(1s, [this] { tick(); }, true);
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
    if (!req.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return reply_bytes(false, "not a forge.v1.HubRequest", instance_);
    }
    if (req.has_register_()) {
        const auto r = on_register(req.register_());
        return r.SerializeAsString();
    }
    if (req.has_unregister()) {
        on_unregister(req.unregister());
        return reply_bytes(true, "", instance_);
    }
    return reply_bytes(false, "empty request", instance_);
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
        reply.set_error("no process " + std::to_string(pid) + " on this machine (the hub only manages local applications)");
        return reply;
    }
    // Everything slow happens before taking the lock: /proc and systemctl.
    std::string launch_error;
    auto launch = read_launch(pid, &launch_error);
    std::string unit;
    if (use_systemd_) {
        const auto u = forge::hub::unit_of(pid);
        // Only if it is the unit's main process (not e.g. a child of a CI runner's service).
        if (valid_unit_name(u)) {
            const auto r = run_command({"systemctl", "show", "-p", "MainPID", "--value", u}, 3s);
            if (r.exit_code == 0 && r.output == std::to_string(pid)) unit = u;
        }
    }

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
    // The same process registering again (retry_s heartbeat). A process the hub
    // started is still STARTING until its first registration.
    const bool same = e.pid == pid && e.ticks == ticks &&
                      (e.state == v1::AppStatus::STATE_RUNNING || e.state == v1::AppStatus::STATE_UNRESPONSIVE);
    if (!same) {
        if (e.restart_at != 0) {  // restarted before the hub did it (systemd)
            e.restart_at = 0;
            count_restart(e);
        }
        if (e.pid != pid) e.child = (e.child == pid) ? pid : 0;
        e.pid = pid;
        e.ticks = ticks;
        e.failing_since = 0;
        e.next_probe = 0;
        e.start_deadline = 0;
        e.stop_requested = false;
        e.start_after_exit = false;
        e.pending_reason.clear();
        e.state = v1::AppStatus::STATE_RUNNING;
        e.message.clear();
        clear_alarm(e);
        logger()->info("registered: {} {} (pid {}{}{})", name, app.version(), pid,
                       app.http_port() ? ", http " + std::to_string(app.http_port()) : std::string(),
                       unit.empty() ? std::string() : ", " + unit);
    }
    e.app = app;
    if (launch) {
        e.launch = std::move(launch);
        e.launch_error.clear();
    } else if (!e.launch) {
        e.launch_error = launch_error;
    }
    e.unit = unit;
    e.last_seen_ms = unix_ms();
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
    e.failing_since = 0;
    if (u.reason() == v1::Unregister::REASON_SIGNAL) {
        e.stop_requested = true;  // systemctl stop, Ctrl-C, kill: never restarted
        process_gone(e, "stopped (stop request)", false);
    } else if (u.exit_code() == 0) {
        process_gone(e, "exited", false);
    } else {
        process_gone(e, "exited with code " + std::to_string(u.exit_code()), true);
    }
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// supervision (main loop)

std::string Hub::title_of(const Entry& e) const { return e.app.title().empty() ? e.app.name() : e.app.title(); }

void Hub::raise(const Entry& e, forge::Severity severity, const std::string& text) {
    alarms().raise("app." + e.app.name(), severity, title_of(e) + ": " + text);
}

void Hub::clear_alarm(const Entry& e) { alarms().clear("app." + e.app.name()); }

std::uint32_t Hub::restarts_in_window(const Entry& e) const {
    const auto window = static_cast<std::int64_t>(std::max<std::uint32_t>(e.app.policy().window_s(), 1)) * 1000;
    const auto now = unix_ms();
    return static_cast<std::uint32_t>(
        std::count_if(e.restarts.begin(), e.restarts.end(), [&](std::int64_t t) { return now - t < window; }));
}

void Hub::count_restart(Entry& e) {
    e.restarts.push_back(unix_ms());
    while (e.restarts.size() > 1000) e.restarts.pop_front();
    ++e.restarts_total;
}

void Hub::process_gone(Entry& e, const std::string& why, bool failure) {
    e.cpu = 0;
    e.rss = 0;
    e.uptime_s = 0;
    e.start_deadline = 0;
    e.kill_at = 0;
    if (e.stop_requested) {
        e.stop_requested = false;
        e.state = v1::AppStatus::STATE_STOPPED;
        e.message = why;
        e.restart_at = 0;
        clear_alarm(e);
        logger()->info("{}: {}", e.app.name(), why);
        if (e.start_after_exit) {
            e.start_after_exit = false;
            start_app(e, "restarting on request");
        }
        return;
    }
    const auto mode = e.app.policy().mode();
    const bool restart = failure ? (mode == v1::RestartPolicy::MODE_ON_FAILURE || mode == v1::RestartPolicy::MODE_ALWAYS)
                                 : mode == v1::RestartPolicy::MODE_ALWAYS;
    e.state = failure ? v1::AppStatus::STATE_CRASHED : v1::AppStatus::STATE_STOPPED;
    e.message = why;
    if (failure) {
        logger()->warn("{}: {}", e.app.name(), why);
        raise(e, forge::Severity::warning, why);
    } else {
        logger()->info("{}: {}", e.app.name(), why);
    }
    if (!restart) return;
    if (!e.launch && e.unit.empty()) {
        give_up(e, why + "; cannot restart it: " + (e.launch_error.empty() ? "no launch information" : e.launch_error));
        return;
    }
    const auto max = e.app.policy().max_restarts();
    if (max > 0 && restarts_in_window(e) >= max) {
        give_up(e, fmt::format("{}; gave up after {} restart(s) in {} s", why, restarts_in_window(e),
                               e.app.policy().window_s()));
        return;
    }
    const auto delay = std::max<std::int64_t>(e.app.policy().delay_s() * 1000LL, 200);
    e.restart_at = steady_ms() + delay;
    e.restart_wait_since = 0;
    e.message = fmt::format("{}; restarting in {} s", why, delay / 1000.0);
}

void Hub::give_up(Entry& e, const std::string& why) {
    e.state = v1::AppStatus::STATE_FAILED;
    e.message = why;
    e.restart_at = 0;
    raise(e, e.app.policy().critical() ? forge::Severity::critical : forge::Severity::error, why);
    logger()->error("{}: {}", e.app.name(), why);
    // A systemd unit restarts by itself too: stop it so "do not run it" holds.
    if (!e.unit.empty()) systemctl(e.app.name(), {"stop"}, e.unit);
}

void Hub::restart_now(Entry& e) {
    const auto now = steady_ms();
    if (e.pid > 0 && alive(e.pid, e.ticks)) {
        // The old process is still on its way out: wait for it (at most 10 s, then SIGKILL).
        if (e.restart_wait_since == 0) e.restart_wait_since = now;
        if (now - e.restart_wait_since > 10000) ::kill(e.pid, SIGKILL);
        e.restart_at = now + 500;
        return;
    }
    count_restart(e);
    start_app(e, fmt::format("restart {} of {}", restarts_in_window(e),
                             e.app.policy().max_restarts() ? std::to_string(e.app.policy().max_restarts()) : "unlimited"));
}

void Hub::start_app(Entry& e, const std::string& why) {
    e.state = v1::AppStatus::STATE_STARTING;
    e.message = why;
    e.start_deadline = steady_ms() + start_timeout_s_ * 1000LL;
    e.failing_since = 0;
    if (!e.unit.empty()) {
        e.pid = 0;  // the new main process registers itself
        // Started already (systemd's own restart)? Then only wait for it.
        systemctl(e.app.name(), {"is-active", "restart"}, e.unit);
        logger()->info("{}: {} (systemd {})", e.app.name(), why, e.unit);
        return;
    }
    if (!e.launch) {
        give_up(e, "cannot start it: " + (e.launch_error.empty() ? "no launch information" : e.launch_error));
        return;
    }
    const auto log_file = log_dir_.empty() ? std::string() : log_dir_ + "/" + e.app.name() + ".log";
    std::string error;
    const int pid = spawn(*e.launch, log_file, &error);
    if (pid < 0) {
        give_up(e, "cannot start it: " + error);
        return;
    }
    e.pid = pid;
    e.ticks = start_ticks(pid);
    e.child = pid;
    logger()->info("{}: {} (pid {})", e.app.name(), why, pid);
}

void Hub::systemctl(const std::string& name, std::vector<std::string> verbs, const std::string& unit) {
    pool().detach_task([this, name, verbs, unit] {
        std::string error;
        for (const auto& verb : verbs) {
            const auto r = run_command({"systemctl", verb, unit}, 30s);
            if (verb == "is-active") {
                if (r.output == "active" || r.output == "activating" || r.output == "reloading") return;
                continue;
            }
            if (r.exit_code != 0) error = fmt::format("systemctl {} {} failed: {}", verb, unit, r.output);
        }
        if (error.empty()) return;
        std::lock_guard lock(mutex_);
        auto it = apps_.find(name);
        if (it == apps_.end()) return;
        logger()->error("{}", error);
        if (it->second.state == v1::AppStatus::STATE_STARTING) give_up(it->second, error);
    });
}

void Hub::tick() {
    struct Probe {
        std::string name;
        int pid;
        std::string url, control;
    };
    std::vector<Probe> probes;
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
            int status = 0;
            std::string exit_info;
            if (e.child > 0 && reap(e.child, &status)) {
                exit_info = describe_exit(status);
                e.child = 0;
            }
            if (live(e.state) && e.pid > 0 && !alive(e.pid, e.ticks)) {
                std::string why = !e.pending_reason.empty() ? e.pending_reason
                                  : !exit_info.empty()      ? exit_info + " without unregistering"
                                                            : "ended without unregistering";
                e.pending_reason.clear();
                process_gone(e, why, true);
                dirty_ = true;
            }
            if (e.state == v1::AppStatus::STATE_STARTING && e.start_deadline && now >= e.start_deadline) {
                e.start_deadline = 0;
                if (e.pid > 0 && alive(e.pid, e.ticks)) {
                    e.state = v1::AppStatus::STATE_RUNNING;
                    e.message = "running, but it did not register";
                } else {
                    process_gone(e, fmt::format("did not start within {} s", start_timeout_s_), true);
                }
                dirty_ = true;
            }
            if (e.restart_at && now >= e.restart_at) {
                e.restart_at = 0;
                restart_now(e);
                dirty_ = true;
            }
            if (e.kill_at && now >= e.kill_at) {
                e.kill_at = 0;
                if (e.pid > 0 && alive(e.pid, e.ticks)) ::kill(e.pid, SIGKILL);
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
            const auto hang = e.app.policy().hang_timeout_s();
            if (hang > 0 && e.failing_since && now - e.failing_since >= hang * 1000LL) {
                e.pending_reason = fmt::format("hung ({} for {} s), killed", e.failing_why, hang);
                logger()->warn("{}: {}", name, e.pending_reason);
                e.failing_since = 0;
                ::kill(e.pid, SIGKILL);
                continue;
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
        if (dirty_) save_registry();
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
                    const auto base = p.url.substr(0, path_start);
                    const auto path = path_start == std::string::npos ? std::string("/") : p.url.substr(path_start);
                    forge::http::Client c(base);
                    c.timeout(timeout);
                    health = c.get_json(path);
                    ok = true;
                } else {
                    forge::zmq::Client c(p.control, timeout);
                    c.set_client("forge-hub");
                    v1::Request req;
                    req.mutable_health();
                    const auto h = c.call(req).health();
                    v1::Request areq;
                    areq.mutable_alarms();
                    const auto alarms = c.call(areq).alarms();
                    int active = 0;
                    for (const auto& a : alarms.alarms()) active += a.active() ? 1 : 0;
                    const auto state = forge::to_string(static_cast<forge::HealthState>(h.state() ? h.state() : 1));
                    health = {{"status", "ok"}, {"health", state}, {"alarms", {{"active", active}}}};
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
    } else {
        why = error.empty() ? "no answer" : error;
    }

    if (ok) {
        e.health = health.value("health", e.static_entry ? "ok" : "");
        e.uptime_s = health.value("uptime_s", std::uint64_t{0});
        if (health.contains("alarms") && health["alarms"].is_object()) {
            const auto& a = health["alarms"];
            e.alarms_active = a.value("active", 0u);
            e.alarms_highest = a.contains("highest") && a["highest"].is_string() ? a["highest"].get<std::string>() : "";
        }
    }
    if (why.empty()) {
        e.last_seen_ms = unix_ms();
        e.failing_since = 0;
        if (e.state == v1::AppStatus::STATE_UNRESPONSIVE || (e.static_entry && e.state != v1::AppStatus::STATE_RUNNING)) {
            e.state = v1::AppStatus::STATE_RUNNING;
            e.message.clear();
            if (!e.static_entry) clear_alarm(e);
        }
        return;
    }
    if (!e.failing_since) e.failing_since = now;
    e.failing_why = why;
    if (now - e.failing_since >= unresponsive_after_s_ * 1000LL && e.state != v1::AppStatus::STATE_UNRESPONSIVE) {
        e.state = v1::AppStatus::STATE_UNRESPONSIVE;
        e.message = why;
        if (!e.static_entry) {
            logger()->warn("{}: unresponsive: {}", name, why);
            raise(e, forge::Severity::warning, "unresponsive: " + why);
        }
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
    s.set_restarts(e.restarts_total);
    s.set_restarts_in_window(restarts_in_window(e));
    s.set_supervision(e.static_entry       ? "none"
                      : !e.unit.empty()    ? "systemd " + e.unit
                      : e.launch           ? "exec"
                                           : "none");
    s.set_last_seen_unix_ms(e.last_seen_ms);
    s.set_url(e.url);
    s.set_static_entry(e.static_entry);
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
    if (it == apps.end()) throw forge::http::HttpError(404, "unknown application '" + name + "'");
    if (it->second.static_entry) {
        throw forge::http::HttpError(409, "'" + name + "' is a static entry: the hub does not manage it");
    }
    return it->second;
}
}  // namespace

json Hub::restart(const std::string& name) {
    std::lock_guard lock(mutex_);
    Entry& e = find_or_throw(apps_, name);
    e.restarts.clear();  // an operator decision starts a new count
    e.restart_at = 0;
    clear_alarm(e);
    if (live(e.state) && e.pid > 0 && alive(e.pid, e.ticks)) {
        if (!e.unit.empty()) {
            start_app(e, "restart requested");
        } else {
            e.stop_requested = true;
            e.start_after_exit = true;
            e.kill_at = steady_ms() + 10000;
            ::kill(e.pid, SIGTERM);
            e.message = "restart requested: stopping";
        }
    } else {
        start_app(e, "started on request");
    }
    dirty_ = true;
    return {{"name", name}, {"state", v1::AppStatus::State_Name(e.state).substr(6)}};
}

json Hub::stop_app(const std::string& name) {
    std::lock_guard lock(mutex_);
    Entry& e = find_or_throw(apps_, name);
    e.restart_at = 0;
    if (!live(e.state) || e.pid <= 0) {
        if (e.state == v1::AppStatus::STATE_CRASHED) e.state = v1::AppStatus::STATE_STOPPED;  // cancel the restart
        throw forge::http::HttpError(409, "'" + name + "' is not running");
    }
    e.stop_requested = true;
    e.start_after_exit = false;
    if (!e.unit.empty()) {
        systemctl(name, {"stop"}, e.unit);
    } else {
        ::kill(e.pid, SIGTERM);
        e.kill_at = steady_ms() + 10000;
    }
    e.message = "stopping";
    dirty_ = true;
    return {{"name", name}, {"state", "stopping"}};
}

json Hub::start(const std::string& name) {
    std::lock_guard lock(mutex_);
    Entry& e = find_or_throw(apps_, name);
    if (live(e.state) && e.pid > 0 && alive(e.pid, e.ticks)) {
        throw forge::http::HttpError(409, "'" + name + "' is already running");
    }
    if (e.unit.empty() && !e.launch) {
        throw forge::http::HttpError(409, "the hub does not know how to start '" + name + "': " + e.launch_error);
    }
    e.restarts.clear();
    e.restart_at = 0;
    clear_alarm(e);
    start_app(e, "started on request");
    dirty_ = true;
    return {{"name", name}, {"state", v1::AppStatus::State_Name(e.state).substr(6)}};
}

json Hub::forget(const std::string& name) {
    std::lock_guard lock(mutex_);
    Entry& e = find_or_throw(apps_, name);
    if (live(e.state) && e.pid > 0 && alive(e.pid, e.ticks)) {
        throw forge::http::HttpError(409, "'" + name + "' is running; stop it first");
    }
    clear_alarm(e);
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
                        {"launch", e.launch ? launch_json(*e.launch) : json(nullptr)},
                        {"launch_error", e.launch_error},
                        {"unit", e.unit},
                        {"pid", e.pid},
                        {"ticks", e.ticks},
                        {"state", static_cast<int>(e.state)},
                        {"message", e.message},
                        {"restarts", e.restarts},
                        {"restarts_total", e.restarts_total}});
    }
    const json doc{{"boot_id", boot_id_}, {"apps", std::move(apps)}};
    try {
        const std::filesystem::path path(state_file_);
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        const auto tmp = state_file_ + ".tmp";
        {
            // The environment of the applications is in here: owner only.
            const auto old = ::umask(077);
            std::ofstream out(tmp, std::ios::trunc);
            ::umask(old);
            out << doc.dump(1);
            if (!out) throw std::runtime_error("write failed");
        }
        ::chmod(tmp.c_str(), 0600);
        std::filesystem::rename(tmp, path);
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
    const bool same_boot = doc.value("boot_id", "") == boot_id_ && !boot_id_.empty();
    std::lock_guard lock(mutex_);
    for (const auto& j : doc.value("apps", json::array())) {
        Entry e;
        try {
            forge::proto::from_json_string(j.at("app").dump(), &e.app);
        } catch (const std::exception&) {
            continue;
        }
        if (!valid_name(e.app.name()) || apps_.count(e.app.name())) continue;
        if (j.contains("launch") && j["launch"].is_object()) e.launch = launch_from(j["launch"]);
        e.launch_error = j.value("launch_error", "");
        e.unit = j.value("unit", "");
        e.restarts = j.value("restarts", std::deque<std::int64_t>{});
        e.restarts_total = j.value("restarts_total", 0u);
        e.message = j.value("message", "");
        const auto state = static_cast<v1::AppStatus::State>(j.value("state", 0));
        if (!same_boot) {
            // The device restarted: whatever starts the applications at boot does
            // so again; the hub restarts nothing from an earlier boot.
            e.state = v1::AppStatus::STATE_UNKNOWN;
            e.message = "not running since the device started";
        } else {
            e.pid = j.value("pid", 0);
            e.ticks = j.value("ticks", std::uint64_t{0});
            e.state = state;
        }
        const auto name = e.app.name();
        Entry& stored = apps_[name] = std::move(e);
        if (!same_boot) continue;
        if (live(state)) {
            if (alive(stored.pid, stored.ticks)) {
                stored.state = v1::AppStatus::STATE_RUNNING;  // checked again right away
            } else {
                process_gone(stored, "ended while the hub was not running", true);
            }
        } else if (state == v1::AppStatus::STATE_CRASHED) {
            process_gone(stored, "crashed (restart pending when the hub stopped)", true);
        }
    }
    logger()->info("registry: {} application(s) from {}{}", apps_.size(), state_file_,
                   same_boot ? "" : " (earlier boot: nothing is restarted)");
}

}  // namespace forgehub
