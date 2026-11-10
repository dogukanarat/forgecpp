#include <forge/forge.hpp>
#include <forge/testing.hpp>

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <fstream>
#include <future>

using namespace std::chrono_literals;

namespace {

struct Tick {
    int n;
};

class TestApp : public forge::Application {
public:
    TestApp() : Application({"testapp", "9.9.9", "test application"}) {}

    int started = 0, stopped = 0, ticks = 0, seen_events = 0;
    std::atomic<int> reloaded{0};  // read by the test while the application runs (SIGHUP)
    std::string greeting;
    int custom_flag = 0;

protected:
    void setup_cli(CLI::App& cli) override { cli.add_option("--custom", custom_flag); }

    void setup_http(forge::http::Server& server) override {
        server.get_json("/api/greeting",
                        [this](const auto&) { return forge::json{{"greeting", invoke([this] { return greeting; })}}; });
    }

    void on_start() override {
        ++started;
        greeting = config().get_or<std::string>("app.greeting", "none");
        sub_ = events().subscribe<Tick>([this](const Tick&) { ++seen_events; });
        every(5ms, [this] {
            ++ticks;
            emit(Tick{ticks});
        });
    }
    void on_reload(const forge::Config&) override { ++reloaded; }
    void on_stop() override { ++stopped; }

private:
    forge::EventBus::Subscription sub_;
};

/// A component whose on_stop() fails; the one added after it must still stop.
class Faulty : public forge::Component {
public:
    Faulty() : Component("faulty") {}

protected:
    void on_stop() override { throw std::runtime_error("device gone"); }
};

class Tidy : public forge::Component {
public:
    Tidy(int& stopped) : Component("tidy"), stopped_(stopped) {}

protected:
    void on_stop() override { ++stopped_; }

private:
    int& stopped_;
};

std::vector<char*> make_argv(std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    return argv;
}

}  // namespace

TEST_CASE("application: full lifecycle with config, overrides, timers, events and http") {
    forge::testing::TempDir dir;
    const auto cfg_path = dir / "testapp.toml";
    std::ofstream(cfg_path) << "[app]\ngreeting = \"from file\"\n[log]\nconsole = false\n";

    std::vector<std::string> args{"testapp", "-c",     cfg_path.string(), "-D", "app.greeting=overridden", "--custom",
                                  "5",       "--http", "127.0.0.1:0",     "-D", "threads.workers=2"};
    auto argv = make_argv(args);

    TestApp app;
    std::promise<int> port;
    app.hook_start([&](forge::Application& a) {
        // Posted tasks run once the main loop is up (after the server listens).
        a.post([&a, &port] { port.set_value(a.http()->port()); });
    });

    std::thread runner([&] {
        const int rc = app.run(static_cast<int>(args.size()), argv.data());
        CHECK(rc == 3);
    });

    const int p = port.get_future().get();
    REQUIRE(p > 0);

    forge::http::Client client("http://127.0.0.1:" + std::to_string(p));
    const auto health = client.get_json("/health");
    CHECK(health["status"] == "ok");
    CHECK(health["version"] == "9.9.9");
    CHECK(client.get_json("/api/greeting")["greeting"] == "overridden");

    CHECK(forge::testing::wait_until([&] { return app.invoke([&] { return app.seen_events; }) >= 3; }));
    CHECK(app.pool().get_thread_count() == 2);

    app.reload();
    CHECK(forge::testing::wait_until([&] { return app.invoke([&] { return app.reloaded.load(); }) == 1; }));

    app.stop(3);
    runner.join();

    CHECK(app.started == 1);
    CHECK(app.stopped == 1);
    CHECK(app.custom_flag == 5);
    CHECK(app.config().get_or<int>("http.port", -1) == 0);
}

TEST_CASE("application: lambda style and startup failure") {
    std::vector<std::string> args{"lambda", "-D", "log.console=false"};
    auto argv = make_argv(args);

    forge::Application app({"lambda"});
    bool stop_called = false;
    app.hook_start([](forge::Application&) { throw std::runtime_error("cannot start"); });
    app.hook_stop([&](forge::Application&) { stop_called = true; });
    CHECK(app.run(static_cast<int>(args.size()), argv.data()) == 1);
    CHECK(stop_called);
}

TEST_CASE("application: bad config file is reported") {
    forge::testing::TempDir dir;
    const auto cfg_path = dir / "bad.toml";
    std::ofstream(cfg_path) << "this is not = = toml\n";
    std::vector<std::string> args{"bad", "--config", cfg_path.string()};
    auto argv = make_argv(args);
    forge::Application app({"bad"});
    CHECK(app.run(static_cast<int>(args.size()), argv.data()) == 1);
}

TEST_CASE("application: the lambda hooks add options, configuration keys and controls") {
    forge::Application app({"hooked", "1.0.0", "No subclass"});
    int gain = 0;
    double limit = 0;
    app.hook_cli([&](forge::Application&, CLI::App& cli) { cli.add_option("--gain", gain); });
    app.hook_config([](forge::Application&, forge::config::Schema& s) {
        s.section("hooked", "Declared by a hook");
        s.number("hooked.limit", 2.5, "A limit");
    });
    app.hook_controls(
        [&](forge::Application&, forge::Controls& ui) { ui.group("Hooked").value("gain", "Gain", gain); });
    app.hook_start([&](forge::Application& a) {
        limit = a.config().value<double>("hooked.limit");
        a.stop(a.dashboard()->state().contains("gain") ? 0 : 3);
    });
    const char* argv[] = {"hooked", "--gain", "7", "-D", "http.enabled=false", "-D", "log.console=false"};
    CHECK(app.run(7, const_cast<char**>(argv)) == 0);
    CHECK(gain == 7);
    CHECK(limit == doctest::Approx(2.5));
}

TEST_CASE("application: a stop handler that throws does not skip the others") {
    int tidy_stopped = 0;
    auto app = std::make_unique<TestApp>();
    app->add_component<Tidy>(tidy_stopped);  // stopped after Faulty (reverse order)
    app->add_component<Faulty>();
    int hook_stopped = 0;
    app->hook_stop([&](forge::Application&) { ++hook_stopped; });
    TestApp* raw = app.get();
    forge::testing::AppRunner<TestApp> r({"-D", "http.enabled=false"}, std::move(app));
    r.stop();
    CHECK(raw->stopped == 1);
    CHECK(tidy_stopped == 1);
    CHECK(hook_stopped == 1);
}

TEST_CASE("application: SIGHUP reloads, SIGTERM stops") {
    // run() blocks the signals on its own thread, which in a daemon is the main
    // thread, so every other thread inherits the mask. Here run() is on the
    // runner's thread: the test thread blocks them itself, or kill() would
    // deliver the signal to it and end the test program.
    struct BlockedSignals {
        BlockedSignals() {
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, SIGHUP);
            sigaddset(&set, SIGTERM);
            pthread_sigmask(SIG_BLOCK, &set, &before);
        }
        ~BlockedSignals() { pthread_sigmask(SIG_SETMASK, &before, nullptr); }
        sigset_t before{};
    } blocked;
    auto app = std::make_unique<TestApp>();
    TestApp* raw = app.get();
    forge::testing::AppRunner<TestApp> r({"-D", "http.enabled=false"}, std::move(app));
    REQUIRE(::kill(::getpid(), SIGHUP) == 0);
    CHECK(forge::testing::wait_until([&] { return raw->reloaded == 1; }));
    CHECK_FALSE(raw->stopping());
    REQUIRE(::kill(::getpid(), SIGTERM) == 0);
    CHECK(forge::testing::wait_until([&] { return raw->stopping(); }));
    CHECK(r.stop() == 0);
    CHECK(raw->stopped == 1);
}
