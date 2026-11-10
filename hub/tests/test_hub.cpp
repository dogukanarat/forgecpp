// forge-hub: registration, health, crash / stop / exit reporting, systemd
// units (state, restarts, policy, actions) through a fake systemd,
// persistence across hub restarts and boots, static entries.
//
// Fast by design: the hub ticks every 20 ms, "applications" are mostly sleep
// processes registered by hand; one test runs a real forgecpp application.
#include "hub.hpp"

#include <forge/hub.hpp>
#include <forge/testing.hpp>

#include <httplib.h>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <set>

extern char** environ;

using namespace std::chrono_literals;
using forge::json;
using forge::pb::view;
using forgehub::UnitState;

namespace {

/// systemd as the tests want it: units, their main pids and the actions asked for.
struct FakeState {
    std::mutex m;
    std::map<int, std::string> unit_by_pid;
    std::map<std::string, UnitState> units;
    std::vector<std::string> actions;
};

class FakeSystemd final : public forgehub::Systemd {
public:
    explicit FakeSystemd(std::shared_ptr<FakeState> s) : s_(std::move(s)) {}
    std::string unit_of(int pid) override {
        std::lock_guard lock(s_->m);
        auto it = s_->unit_by_pid.find(pid);
        return it == s_->unit_by_pid.end() ? std::string() : it->second;
    }
    std::map<std::string, UnitState> show(const std::vector<std::string>& units) override {
        std::lock_guard lock(s_->m);
        std::map<std::string, UnitState> out;
        for (const auto& u : units) {
            if (s_->units.count(u)) out[u] = s_->units[u];
        }
        return out;
    }
    bool action(const std::string& verb, const std::string& unit, std::string*) override {
        std::lock_guard lock(s_->m);
        s_->actions.push_back(verb + " " + unit);
        auto& u = s_->units[unit];
        if (verb == "stop") {
            u.active = "inactive";
            u.sub = "dead";
        } else if (verb == "reset-failed" && u.active == "failed") {
            u.active = "inactive";
        } else if (verb == "start" || verb == "restart") {
            u.active = "activating";
            u.sub = "start";
        }
        return true;
    }

private:
    std::shared_ptr<FakeState> s_;
};

UnitState unit(const std::string& active, const std::string& sub, int main_pid, std::uint32_t restarts = 0,
               const std::string& result = "success") {
    UnitState u;
    u.loaded = true;
    u.active = active;
    u.sub = sub;
    u.main_pid = main_pid;
    u.n_restarts = restarts;
    u.result = result;
    u.restart = "on-failure";
    u.start_limit_burst = 4;
    u.start_limit_interval_us = 600'000'000;
    u.restart_us = 2'000'000;
    u.watchdog_us = 30'000'000;
    return u;
}

struct HubFixture {
    forge::testing::TempDir dir;
    std::string boot_file = (dir / "boot_id").string();
    std::string state_file = (dir / "registry.json").string();
    std::string app_config = (dir / "app.toml").string();
    std::shared_ptr<FakeState> sd = std::make_shared<FakeState>();
    std::set<int> children;
    std::unique_ptr<forge::testing::AppRunner<forgehub::Hub>> hub;

    HubFixture(std::vector<std::string> extra = {}) {
        set_boot("boot-1");
        std::ofstream(app_config) << "# test application\n";
        start_hub(std::move(extra));
    }
    ~HubFixture() {
        if (hub) hub->stop();
        for (int p : children) ::kill(p, SIGKILL);
        for (int p : children) ::waitpid(p, nullptr, 0);
    }
    void set_boot(const std::string& id) { std::ofstream(boot_file) << id << "\n"; }
    void start_hub(std::vector<std::string> extra = {}) {
        std::vector<std::string> args{
            "-D", "registry.listen=tcp://127.0.0.1:*",  "-D", "registry.state_file=" + state_file,
            "-D", "registry.boot_id_file=" + boot_file, "-D", "registry.tick_ms=20",
            "-D", "registry.probe_interval_ms=50",      "-D", "registry.probe_timeout_ms=300",
            "-D", "registry.unresponsive_after_s=0",    "-D", "http.enabled=false"};
        args.insert(args.end(), extra.begin(), extra.end());
        hub = std::make_unique<forge::testing::AppRunner<forgehub::Hub>>(
            args, std::make_unique<forgehub::Hub>(std::make_unique<FakeSystemd>(sd)));
    }
    void stop_hub() {
        hub->stop();
        hub.reset();
    }
    forgehub::Hub& h() { return hub->app(); }

    int spawn(std::vector<std::string> args) {
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        pid_t pid = 0;
        REQUIRE(::posix_spawn(&pid, args[0].c_str(), nullptr, nullptr, argv.data(), environ) == 0);
        children.insert(pid);
        return pid;
    }
    /// A process that only exists (registered by hand).
    int sleeper() { return spawn({"/bin/sleep", "60"}); }
    void kill_child(int pid) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        children.erase(pid);
    }
    /// The real thing: a forgecpp application registering by itself.
    int application(const std::string& name, std::vector<std::string> extra = {}) {
        std::vector<std::string> args{HUB_TEST_APP,
                                      "--name",
                                      name,
                                      "-c",
                                      app_config,
                                      "-D",
                                      "log.console=false",
                                      "-D",
                                      "http.enabled=true",
                                      "-D",
                                      "http.host=127.0.0.1",
                                      "-D",
                                      "http.port=0",
                                      "-D",
                                      "hub.endpoint=" + h().registry_endpoint(),
                                      "-D",
                                      "hub.retry_s=1"};
        args.insert(args.end(), extra.begin(), extra.end());
        return spawn(args);
    }

    forge::Status reg(const std::string& name, int pid, bool critical = false) {
        forge_hub_app_registration a{};
        REQUIRE(forge::pb::assign(a.name, name).ok());
        REQUIRE(forge::pb::assign(a.version, "1.0").ok());
        a.pid = static_cast<std::uint32_t>(pid);
        a.critical = critical;
        forge_hub_register_reply reply{};
        return forge::hub::register_app(h().registry_endpoint(), a, reply, 2s);
    }
    void set_unit(const std::string& name, const UnitState& u) {
        std::lock_guard lock(sd->m);
        sd->units[name] = u;
    }
    std::vector<std::string> actions() {
        std::lock_guard lock(sd->m);
        return sd->actions;
    }

    std::optional<forgehub::Entry> entry(const std::string& name) { return h().entry(name); }
    bool wait_state(const std::string& name, forge_hub_app_state s, std::chrono::milliseconds t = 5s) {
        return forge::testing::wait_until([&] {
            const auto e = entry(name);
            return e && e->state == s;
        }, t);
    }
    forge_hub_app_status status(const std::string& name) {
        const auto l = h().list();
        for (const auto& a : l.apps) {
            if (view(a.app.name) == name) return a;
        }
        FAIL("no status for " << name);
        return {};
    }
};

}  // namespace

TEST_CASE("hub: a forgecpp application registers by itself; health; stop request and exit code") {
    HubFixture f;
    const int a = f.application("alpha", {"-D", "hub.critical=true", "-D", "hub.group=Radio", "-D", "hub.title=Alpha"});
    REQUIRE(f.wait_state("alpha", forge_hub_app_state_STATE_RUNNING));
    REQUIRE(forge::testing::wait_until([&] { return f.entry("alpha")->health == "ok"; }));
    const auto e = *f.entry("alpha");
    CHECK(e.pid == a);
    CHECK(view(e.app.version) == "1.2.3");
    CHECK(e.app.http_port > 0);
    CHECK(view(e.app.group) == "Radio");
    CHECK(view(e.app.title) == "Alpha");
    CHECK(e.app.critical);
    CHECK(view(f.status("alpha").supervision) == "none");
    CHECK(forge::testing::wait_until([&] { return f.status("alpha").rss_bytes > 0; }));

    // SIGTERM (systemctl stop, Ctrl-C): the application unregisters as stopped.
    ::kill(a, SIGTERM);
    REQUIRE(f.wait_state("alpha", forge_hub_app_state_STATE_STOPPED));
    CHECK(f.entry("alpha")->message == "stopped");
    CHECK_FALSE(f.h().alarms().active("app.alpha.ended"));

    // An error exit is a failure (reported; without a unit nothing restarts it).
    f.application("beta", {"--exit-after-ms", "300", "--exit-code", "3"});
    REQUIRE(f.wait_state("beta", forge_hub_app_state_STATE_CRASHED));
    CHECK(f.entry("beta")->message == "exited with code 3 (not a systemd unit: not restarted)");
    CHECK(f.h().alarms().active("app.beta.ended"));
}

TEST_CASE("hub: without a unit a crash is reported, not restarted; duplicates are refused") {
    HubFixture f;
    const int p = f.sleeper();
    REQUIRE(f.reg("gamma", p).ok());
    REQUIRE(f.wait_state("gamma", forge_hub_app_state_STATE_RUNNING));
    const int other = f.sleeper();
    const auto dup = f.reg("gamma", other);
    CHECK(dup.errno_code() == EEXIST);
    CHECK_FALSE(f.reg("bad name!", p).ok());
    CHECK_FALSE(f.reg("ghost", 999999).ok());

    f.kill_child(p);
    REQUIRE(f.wait_state("gamma", forge_hub_app_state_STATE_CRASHED));
    CHECK(f.entry("gamma")->message == "ended without unregistering (not a systemd unit: not restarted)");
    CHECK(f.h().alarms().active("app.gamma.ended"));
    CHECK_THROWS_AS(f.h().restart("gamma"), forge::Error);  // no unit: nothing to restart with
    CHECK_THROWS_AS(f.h().start("gamma"), forge::Error);

    // Registering again (started by someone) clears it.
    REQUIRE(f.reg("gamma", other).ok());
    REQUIRE(f.wait_state("gamma", forge_hub_app_state_STATE_RUNNING));
    CHECK_FALSE(f.h().alarms().active("app.gamma.ended"));
    // The dashboard's stop for a process without a unit: SIGTERM.
    f.h().stop_app("gamma");
    REQUIRE(forge::testing::wait_until([&] { return ::waitpid(other, nullptr, WNOHANG) == other; }, 3s));
    f.children.erase(other);
    f.h().forget("gamma");
    CHECK_FALSE(f.entry("gamma"));
}

TEST_CASE("hub: the applications' pages through the hub's port (/apps/<name>/)") {
    HubFixture f({"-D", "http.enabled=true", "-D", "http.host=127.0.0.1", "-D", "http.port=0"});
    f.application("alpha");
    REQUIRE(f.wait_state("alpha", forge_hub_app_state_STATE_RUNNING));
    httplib::Client c("127.0.0.1", f.h().http()->port());
    c.set_read_timeout(5, 0);
    c.set_decompress(true);  // like a browser: gzip accepted

    auto r = c.Get("/apps/alpha");  // the slash matters for relative URLs
    REQUIRE(r);
    CHECK(r->status == 302);
    CHECK(r->get_header_value("Location") == "/apps/alpha/");
    r = c.Get("/apps/alpha/health?strict=1");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(forge::json::parse(r->body)["name"] == "alpha");
    r = c.Get("/apps/alpha/forge/api/info");
    REQUIRE(r);
    CHECK(forge::json::parse(r->body)["name"] == "alpha");
    r = c.Get("/apps/alpha/");  // the application redirects to its panel: under the prefix
    REQUIRE(r);
    CHECK(r->status == 302);
    CHECK(r->get_header_value("Location") == "forge/");
    r = c.Get("/apps/alpha/forge/forge.js");
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->body.find("forge/forge.js") != std::string::npos);
    r = c.Post("/apps/alpha/forge/api/tmtc/info", "{}", "application/json");
    REQUIRE(r);
    CHECK(forge::json::parse(r->body)["name"] == "alpha");
    r = c.Get("/apps/ghost/");
    REQUIRE(r);
    CHECK(r->status == 502);
    r = c.Get("/api/hub");
    REQUIRE(r);
    CHECK(forge::json::parse(r->body)["proxy"] == true);
}

TEST_CASE("hub: several instances of one application (name@instance)") {
    HubFixture f;
    const int one = f.sleeper(), two = f.sleeper();
    REQUIRE(f.reg("delta@one", one).ok());
    REQUIRE(f.reg("delta@two", two).ok());
    REQUIRE(f.wait_state("delta@one", forge_hub_app_state_STATE_RUNNING));
    REQUIRE(f.wait_state("delta@two", forge_hub_app_state_STATE_RUNNING));
    CHECK(f.entry("delta@one")->pid == one);
    CHECK(f.entry("delta@two")->pid == two);
    for (const auto* bad : {"@one", "delta@", "a@b@c"}) CHECK_FALSE(f.reg(bad, one).ok());
    // One instance ending does not touch the other.
    f.kill_child(one);
    REQUIRE(f.wait_state("delta@one", forge_hub_app_state_STATE_CRASHED));
    CHECK(f.entry("delta@two")->state == forge_hub_app_state_STATE_RUNNING);
    CHECK(f.h().alarms().active("app.delta@one.ended"));
    CHECK_FALSE(f.h().alarms().active("app.delta@two.ended"));

    CHECK(forge::hub::instance_of("radio-controller@rx2.service") == "rx2");
    CHECK(forge::hub::instance_of("radio-controller.service").empty());
    CHECK(forge::hub::instance_of("radio-controller@.service").empty());
}

TEST_CASE("hub: systemd units: state, restarts, policy and why it failed come from systemd") {
    HubFixture f;
    const int p = f.sleeper();
    {
        std::lock_guard lock(f.sd->m);
        f.sd->unit_by_pid[p] = "alpha.service";
    }
    f.set_unit("alpha.service", unit("active", "running", p));
    REQUIRE(f.reg("alpha", p, /*critical=*/true).ok());
    REQUIRE(forge::testing::wait_until([&] { return view(f.status("alpha").unit_state) == "active/running"; }));
    auto s = f.status("alpha");
    CHECK(s.state == forge_hub_app_state_STATE_RUNNING);
    CHECK(view(s.supervision) == "systemd alpha.service");
    CHECK(s.policy.mode == forge_hub_restart_mode_MODE_ON_FAILURE);
    CHECK(s.policy.max_restarts == 3);  // StartLimitBurst 4 = first start + 3 restarts
    CHECK(s.policy.window_s == 600);
    CHECK(s.policy.delay_s == 2);
    CHECK(s.policy.watchdog_s == 30);

    // It crashes; systemd schedules a restart.
    f.kill_child(p);
    f.set_unit("alpha.service", unit("activating", "auto-restart", 0, 1, "signal"));
    REQUIRE(f.wait_state("alpha", forge_hub_app_state_STATE_STARTING));
    REQUIRE(forge::testing::wait_until([&] { return f.status("alpha").restarts == 1; }));
    CHECK(f.entry("alpha")->message == "killed by a signal; systemd restarts it in 2 s");
    CHECK(f.h().alarms().active("app.alpha.ended"));

    // systemd started it again: waiting for it, then it registers.
    const int q = f.sleeper();
    f.set_unit("alpha.service", unit("active", "running", q, 1, "signal"));
    REQUIRE(forge::testing::wait_until(
        [&] { return f.entry("alpha")->message.find("waiting for it to register") != std::string::npos; }));
    {
        std::lock_guard lock(f.sd->m);
        f.sd->unit_by_pid[q] = "alpha.service";
    }
    REQUIRE(f.reg("alpha", q, true).ok());
    REQUIRE(f.wait_state("alpha", forge_hub_app_state_STATE_RUNNING));
    CHECK_FALSE(f.h().alarms().active("app.alpha.ended"));

    // Too many restarts: the unit failed and stays down.
    f.kill_child(q);
    // (As on a real system: an automatic restart refused by the start limit
    // leaves the last run's Result; only a refused manual start says start-limit-hit.)
    f.set_unit("alpha.service", unit("failed", "failed", 0, 4, "signal"));  // the refused one counts too
    REQUIRE(f.wait_state("alpha", forge_hub_app_state_STATE_FAILED));
    CHECK(f.entry("alpha")->message == "gave up after 3 restarts in 600 s (last: killed by a signal)");
    REQUIRE(f.h().alarms().get("app.alpha.failed"));
    CHECK(f.h().alarms().get("app.alpha.failed")->severity == forge::Severity::critical);

    // Operator actions go to systemctl.
    f.h().restart("alpha");
    REQUIRE(forge::testing::wait_until([&] { return f.actions().size() >= 2; }));
    CHECK(f.actions() == std::vector<std::string>{"reset-failed alpha.service", "restart alpha.service"});
    REQUIRE(forge::testing::wait_until([&] { return view(f.status("alpha").unit_state) == "activating/start"; }));
    CHECK_FALSE(f.h().alarms().active("app.alpha.failed"));
    f.h().stop_app("alpha");
    REQUIRE(f.wait_state("alpha", forge_hub_app_state_STATE_STOPPED));
    CHECK(f.actions().back() == "stop alpha.service");
    f.h().start("alpha");
    REQUIRE(forge::testing::wait_until([&] { return f.actions().back() == "start alpha.service"; }));
}

TEST_CASE("hub: the registry survives hub restarts; processes of an earlier boot are gone") {
    HubFixture f;
    const int p = f.sleeper();
    const int u = f.sleeper();
    {
        std::lock_guard lock(f.sd->m);
        f.sd->unit_by_pid[u] = "unit.service";
    }
    f.set_unit("unit.service", unit("active", "running", u));
    REQUIRE(f.reg("plain", p).ok());
    REQUIRE(f.reg("unit", u).ok());
    REQUIRE(f.wait_state("unit", forge_hub_app_state_STATE_RUNNING));
    f.stop_hub();

    // Same boot: both known; the unit's state comes from systemd.
    f.start_hub();
    REQUIRE(f.entry("plain"));
    CHECK(f.entry("plain")->state == forge_hub_app_state_STATE_RUNNING);
    CHECK(f.entry("plain")->pid == p);
    REQUIRE(forge::testing::wait_until([&] { return view(f.status("unit").unit_state) == "active/running"; }));
    f.set_unit("unit.service", unit("inactive", "dead", 0));
    REQUIRE(f.wait_state("unit", forge_hub_app_state_STATE_STOPPED));
    f.stop_hub();

    // Another boot: the processes are gone.
    f.set_boot("boot-2");
    f.start_hub();
    CHECK(f.entry("plain")->state == forge_hub_app_state_STATE_UNKNOWN);
    CHECK(f.entry("plain")->message == "not running");
    REQUIRE(f.wait_state("unit", forge_hub_app_state_STATE_STOPPED));  // systemd knows
}

TEST_CASE("hub: static entries are listed and checked") {
    HubFixture f({"-D", "static.docs.title=Docs", "-D", "static.docs.url=http://{host}:9000/", "-D",
                  "static.dead.health_url=http://127.0.0.1:1/health"});
    REQUIRE(f.entry("docs"));
    CHECK(f.entry("docs")->static_entry);
    CHECK(f.entry("docs")->state == forge_hub_app_state_STATE_UNKNOWN);
    REQUIRE(f.wait_state("dead", forge_hub_app_state_STATE_UNRESPONSIVE));
    CHECK_THROWS_AS(f.h().restart("docs"), forge::Error);
    CHECK(view(f.status("docs").url) == "http://{host}:9000/");
    CHECK(f.status("docs").static_entry);
}

TEST_CASE("hub: systemctl show output and timespans") {
    CHECK(forgehub::timespan_us("2s") == 2'000'000);
    CHECK(forgehub::timespan_us("100ms") == 100'000);
    CHECK(forgehub::timespan_us("10min") == 600'000'000);
    CHECK(forgehub::timespan_us("1min 30s") == 90'000'000);
    CHECK(forgehub::timespan_us("1h 2min 3.5s") == 3'723'500'000ULL);
    CHECK(forgehub::timespan_us("infinity") == 0);
    CHECK(forgehub::timespan_us("0") == 0);
    CHECK(forgehub::timespan_us("123") == 123);
    CHECK(forgehub::timespan_us("bogus") == 0);

    const auto units = forgehub::parse_show(
        "Id=radio-controller.service\nLoadState=loaded\nActiveState=active\nSubState=running\nResult=success\n"
        "MainPID=1234\nNRestarts=2\nRestart=on-failure\nStartLimitBurst=4\nStartLimitIntervalUSec=10min\n"
        "RestartUSec=2s\nWatchdogUSec=30s\n\n"
        "Id=gone.service\nLoadState=not-found\nActiveState=inactive\n");
    REQUIRE(units.size() == 2);
    const auto& r = units.at("radio-controller.service");
    CHECK(r.loaded);
    CHECK(r.active == "active");
    CHECK(r.main_pid == 1234);
    CHECK(r.n_restarts == 2);
    CHECK(r.start_limit_burst == 4);
    CHECK(r.start_limit_interval_us == 600'000'000);
    CHECK(r.restart_us == 2'000'000);
    CHECK(r.watchdog_us == 30'000'000);
    CHECK_FALSE(units.at("gone.service").loaded);
}
