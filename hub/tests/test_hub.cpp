// forge-hub against real processes: registration, health, crashes and the
// restart policy, clean stops, hangs, exits, persistence across hub restarts
// and boots, static entries.
#include "hub.hpp"

#include <forge/testing.hpp>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

#include <fstream>
#include <set>

extern char** environ;

using namespace std::chrono_literals;
using forge::json;
namespace v1 = forge::v1;

namespace {

/// A hub on free ports, with its own state file and boot id.
struct HubFixture {
    forge::testing::TempDir dir;
    std::string boot_file = (dir / "boot_id").string();
    std::string state_file = (dir / "registry.json").string();
    std::string empty = (dir / "app.toml").string();
    std::set<int> spawned;
    std::unique_ptr<forge::testing::AppRunner<forgehub::Hub>> hub;

    HubFixture() {
        set_boot("boot-1");
        std::ofstream(empty) << "# test application\n";
        start_hub();
    }
    ~HubFixture() {
        // Stop every process of the test, including those the hub started.
        std::set<int> pids = spawned;
        if (hub) {
            const auto list = (*hub)->list();
            for (const auto& a : list.apps()) {
                if (a.app().pid()) pids.insert(static_cast<int>(a.app().pid()));
            }
            hub->stop();
        }
        for (int p : pids) ::kill(p, SIGKILL);
        for (int p : pids) ::waitpid(p, nullptr, WNOHANG);
    }
    void set_boot(const std::string& id) { std::ofstream(boot_file) << id << "\n"; }
    void start_hub(std::vector<std::string> extra = {}) {
        std::vector<std::string> args{"-D", "registry.listen=tcp://127.0.0.1:*",
                                      "-D", "registry.state_file=" + state_file,
                                      "-D", "registry.boot_id_file=" + boot_file,
                                      "-D", "registry.probe_interval_ms=200",
                                      "-D", "registry.probe_timeout_ms=500",
                                      "-D", "registry.unresponsive_after_s=1",
                                      "-D", "registry.start_timeout_s=10",
                                      "-D", "registry.systemd=false",
                                      "-D", "registry.log_dir=" + (dir / "logs").string(),
                                      "-D", "http.enabled=true",
                                      "-D", "http.host=127.0.0.1",
                                      "-D", "http.port=0"};
        args.insert(args.end(), extra.begin(), extra.end());
        hub = std::make_unique<forge::testing::AppRunner<forgehub::Hub>>(args);
    }
    forgehub::Hub& h() { return hub->app(); }

    /// Start the test application, registering with this hub.
    int launch(const std::string& name, std::vector<std::string> extra = {}) {
        std::vector<std::string> args{HUB_TEST_APP, "--name", name, "-c", empty,
                                      "-D", "log.console=false", "-D", "http.enabled=true",
                                      "-D", "http.host=127.0.0.1", "-D", "http.port=0",
                                      "-D", "hub.endpoint=" + h().registry_endpoint(), "-D", "hub.retry_s=1"};
        args.insert(args.end(), extra.begin(), extra.end());
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        pid_t pid = 0;
        REQUIRE(::posix_spawn(&pid, HUB_TEST_APP, nullptr, nullptr, argv.data(), environ) == 0);
        spawned.insert(pid);
        return pid;
    }

    std::optional<forgehub::Entry> entry(const std::string& name) { return h().entry(name); }
    bool wait_state(const std::string& name, v1::AppStatus::State s, std::chrono::milliseconds t = 15s) {
        return forge::testing::wait_until([&] {
            const auto e = entry(name);
            return e && e->state == s;
        }, t);
    }
    bool wait_running(const std::string& name, int not_pid = 0, std::chrono::milliseconds t = 15s) {
        return forge::testing::wait_until([&] {
            const auto e = entry(name);
            return e && e->state == v1::AppStatus::STATE_RUNNING && e->pid > 0 && e->pid != not_pid;
        }, t);
    }
    void reap_spawned() {
        for (int p : spawned) ::waitpid(p, nullptr, WNOHANG);
    }
};

std::string state_name(const std::optional<forgehub::Entry>& e) {
    return e ? v1::AppStatus::State_Name(e->state) + " (" + e->message + ")" : "none";
}

}  // namespace

TEST_CASE("hub: an application registers with its ports, policy and launch information; health is checked") {
    HubFixture f;
    const int pid = f.launch("alpha", {"-D", "hub.restart=on-failure", "-D", "hub.max_restarts=2",
                                       "-D", "hub.critical=true", "-D", "hub.group=Radio", "-D", "hub.title=Alpha"});
    REQUIRE(f.wait_running("alpha"));
    REQUIRE(forge::testing::wait_until([&] { return f.entry("alpha")->health == "ok"; }, 10s));
    const auto e = *f.entry("alpha");
    CHECK(e.pid == pid);
    CHECK(e.app.version() == "1.2.3");
    CHECK(e.app.http_port() > 0);
    CHECK(e.app.group() == "Radio");
    CHECK(e.app.title() == "Alpha");
    CHECK(e.app.policy().mode() == v1::RestartPolicy::MODE_ON_FAILURE);
    CHECK(e.app.policy().max_restarts() == 2);
    CHECK(e.app.policy().critical());
    REQUIRE(e.launch);
    CHECK(e.launch->exe == std::string(HUB_TEST_APP));
    CHECK(std::find(e.launch->argv.begin(), e.launch->argv.end(), "alpha") != e.launch->argv.end());
    CHECK(e.unit.empty());

    const auto list = f.h().list();
    REQUIRE(list.apps_size() == 1);
    CHECK(list.apps(0).supervision() == "exec");
    CHECK(list.apps(0).state() == v1::AppStatus::STATE_RUNNING);
    CHECK(list.apps(0).rss_bytes() > 0);

    // A second process with the same name is refused while the first one runs.
    const int dup = f.launch("alpha");
    std::this_thread::sleep_for(1500ms);
    CHECK(f.entry("alpha")->pid == pid);
    ::kill(dup, SIGKILL);
}

TEST_CASE("hub: a crash is restarted with the same arguments; too many crashes and it gives up") {
    HubFixture f;
    // Restarted once by SIGKILL...
    const int pid = f.launch("beta", {"-D", "hub.restart=on-failure", "-D", "hub.max_restarts=2",
                                      "-D", "hub.restart_delay_s=0"});
    REQUIRE(f.wait_running("beta"));
    ::kill(pid, SIGKILL);
    REQUIRE(f.wait_running("beta", pid, 6s));  // registers again long before start_timeout_s (10 s)
    auto e = *f.entry("beta");
    CHECK(e.restarts_total == 1);
    CHECK(e.child == e.pid);  // started by the hub
    CHECK(forgehub::read_launch(e.pid)->argv == f.entry("beta")->launch->argv);
    CHECK_FALSE(f.h().alarms().active("app.beta"));

    // ...killed again: the second restart...
    const int second = e.pid;
    ::kill(second, SIGKILL);
    REQUIRE(f.wait_running("beta", second));
    CHECK(f.entry("beta")->restarts_total == 2);

    // ...and the third crash within the window is one too many.
    ::kill(f.entry("beta")->pid, SIGKILL);
    REQUIRE(f.wait_state("beta", v1::AppStatus::STATE_FAILED));
    e = *f.entry("beta");
    CAPTURE(e.message);
    CHECK(e.message.find("gave up after 2 restart(s)") != std::string::npos);
    CHECK(f.h().alarms().active("app.beta"));
    std::this_thread::sleep_for(1500ms);
    CHECK(f.entry("beta")->state == v1::AppStatus::STATE_FAILED);  // not started again

    // An operator starts it again (with a fresh count; the alarm is cleared).
    f.h().start("beta");
    REQUIRE(f.wait_running("beta"));
    CHECK(f.entry("beta")->restarts.empty());
    CHECK_FALSE(f.h().alarms().active("app.beta"));
}

TEST_CASE("hub: an application that crashes by itself is given up after max_restarts") {
    HubFixture f;
    f.launch("crashy", {"-D", "hub.restart=on-failure", "-D", "hub.max_restarts=2", "-D", "hub.restart_delay_s=0",
                        "--crash-after-ms", "1200"});
    REQUIRE(f.wait_state("crashy", v1::AppStatus::STATE_FAILED, 30s));
    const auto e = *f.entry("crashy");
    CHECK(e.restarts_total == 2);
    CHECK(e.message.find("killed by") != std::string::npos);
    CHECK(f.h().alarms().get("app.crashy")->severity == forge::Severity::error);  // critical = false
}

TEST_CASE("hub: a stop request is not a crash; start brings the application back") {
    HubFixture f;
    const int pid = f.launch("gamma", {"-D", "hub.restart=always"});
    REQUIRE(f.wait_running("gamma"));
    ::kill(pid, SIGTERM);  // like systemctl stop: the application unregisters with REASON_SIGNAL
    REQUIRE(f.wait_state("gamma", v1::AppStatus::STATE_STOPPED));
    std::this_thread::sleep_for(2500ms);
    CHECK_MESSAGE(f.entry("gamma")->state == v1::AppStatus::STATE_STOPPED, state_name(f.entry("gamma")));
    CHECK_FALSE(f.h().alarms().active("app.gamma"));
    f.reap_spawned();

    f.h().start("gamma");
    REQUIRE(f.wait_running("gamma", pid));
    // The dashboard's stop: SIGTERM, stopped, not restarted although the policy is "always".
    f.h().stop_app("gamma");
    REQUIRE(f.wait_state("gamma", v1::AppStatus::STATE_STOPPED));
    std::this_thread::sleep_for(2500ms);
    CHECK(f.entry("gamma")->state == v1::AppStatus::STATE_STOPPED);
    CHECK_THROWS_AS(f.h().stop_app("gamma"), forge::http::HttpError);
}

TEST_CASE("hub: restart from the dashboard; policy never only reports a crash") {
    HubFixture f;
    const int pid = f.launch("delta");  // default policy: never
    REQUIRE(f.wait_running("delta"));
    f.h().restart("delta");
    REQUIRE(f.wait_running("delta", pid));
    const int second = f.entry("delta")->pid;
    f.reap_spawned();

    ::kill(second, SIGKILL);
    REQUIRE(f.wait_state("delta", v1::AppStatus::STATE_CRASHED));
    std::this_thread::sleep_for(2500ms);
    CHECK(f.entry("delta")->state == v1::AppStatus::STATE_CRASHED);
    CHECK(f.h().alarms().active("app.delta"));
    CHECK_THROWS_AS(f.h().restart("nobody"), forge::http::HttpError);
    f.h().forget("delta");
    CHECK_FALSE(f.entry("delta"));
}

TEST_CASE("hub: a hung application is killed and restarted") {
    HubFixture f;
    const int pid = f.launch("hung", {"-D", "hub.restart=on-failure", "-D", "hub.max_restarts=5",
                                      "-D", "hub.restart_delay_s=0", "-D", "hub.hang_timeout_s=2",
                                      "--hang-after-ms", "1500"});
    REQUIRE(f.wait_running("hung"));
    REQUIRE(f.wait_state("hung", v1::AppStatus::STATE_UNRESPONSIVE, 15s));
    CHECK(f.entry("hung")->message.find("main loop stalled") != std::string::npos);
    REQUIRE(forge::testing::wait_until([&] { return f.entry("hung")->restarts_total >= 1; }, 20s));
    REQUIRE(f.wait_running("hung", pid));
}

TEST_CASE("hub: an exit with an error code is a failure, a clean exit is not") {
    HubFixture f;
    f.launch("epsilon", {"-D", "hub.restart=on-failure", "-D", "hub.max_restarts=1", "-D", "hub.restart_delay_s=0",
                         "--exit-after-ms", "800", "--exit-code", "3"});
    REQUIRE(f.wait_state("epsilon", v1::AppStatus::STATE_FAILED, 20s));
    CHECK(f.entry("epsilon")->restarts_total == 1);
    CHECK(f.entry("epsilon")->message.find("exited with code 3") != std::string::npos);

    f.launch("zeta", {"-D", "hub.restart=on-failure", "--exit-after-ms", "800"});
    REQUIRE(f.wait_state("zeta", v1::AppStatus::STATE_STOPPED, 15s));
    std::this_thread::sleep_for(2000ms);
    CHECK(f.entry("zeta")->state == v1::AppStatus::STATE_STOPPED);
    CHECK(f.entry("zeta")->restarts_total == 0);
}

TEST_CASE("hub: the registry survives a hub restart; nothing is restarted after a reboot") {
    HubFixture f;
    const int pid = f.launch("eta", {"-D", "hub.restart=on-failure", "-D", "hub.restart_delay_s=0"});
    REQUIRE(f.wait_running("eta"));
    f.hub->stop();
    f.hub.reset();

    // Same boot: known and still running.
    f.start_hub();
    REQUIRE(f.entry("eta"));
    CHECK(f.entry("eta")->state == v1::AppStatus::STATE_RUNNING);
    CHECK(f.entry("eta")->pid == pid);
    REQUIRE(forge::testing::wait_until([&] { return f.entry("eta")->health == "ok"; }, 10s));
    f.hub->stop();
    f.hub.reset();

    // It crashes while the hub is down: restarted when the hub comes back.
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    f.start_hub();
    // Started again with its old arguments (they name the old hub's port, so it
    // runs without registering: the hub watches the process it started).
    REQUIRE(f.wait_running("eta", pid, 25s));
    const int restarted = f.entry("eta")->pid;
    f.hub->stop();
    f.hub.reset();
    ::kill(restarted, SIGKILL);
    ::waitpid(restarted, nullptr, 0);

    // Another boot: shown as not running, never restarted from the old registry.
    f.set_boot("boot-2");
    f.start_hub();
    REQUIRE(f.entry("eta"));
    CHECK(f.entry("eta")->state == v1::AppStatus::STATE_UNKNOWN);
    std::this_thread::sleep_for(2000ms);
    CHECK(f.entry("eta")->state == v1::AppStatus::STATE_UNKNOWN);
}

TEST_CASE("hub: static entries are listed and checked") {
    HubFixture f;
    f.hub->stop();
    f.hub.reset();
    f.start_hub({"-D", "static.docs.title=Docs", "-D", "static.docs.url=http://{host}:9000/",
                 "-D", "static.dead.health_url=http://127.0.0.1:1/health"});
    REQUIRE(f.entry("docs"));
    CHECK(f.entry("docs")->static_entry);
    CHECK(f.entry("docs")->state == v1::AppStatus::STATE_UNKNOWN);
    REQUIRE(f.wait_state("dead", v1::AppStatus::STATE_UNRESPONSIVE, 10s));
    CHECK_THROWS_AS(f.h().restart("docs"), forge::http::HttpError);
    const auto list = f.h().list();
    REQUIRE(list.apps_size() == 2);  // sorted by name: dead, docs
    CHECK(list.apps(1).app().name() == "docs");
    CHECK(list.apps(1).url() == "http://{host}:9000/");
    CHECK(list.apps(1).static_entry());
}
