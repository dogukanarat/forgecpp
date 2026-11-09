#include <forge/forge.hpp>
#include <forge/testing.hpp>

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

    int started = 0, stopped = 0, reloaded = 0, ticks = 0, seen_events = 0;
    std::string greeting;
    int custom_flag = 0;

protected:
    void setup_cli(CLI::App& cli) override { cli.add_option("--custom", custom_flag); }

    void setup_http(forge::http::Server& server) override {
        server.get_json("/api/greeting", [this](const auto&) {
            return forge::json{{"greeting", call([this] { return greeting; }).get()}};
        });
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

    CHECK(forge::testing::wait_until([&] { return app.call([&] { return app.seen_events; }).get() >= 3; }));
    CHECK(app.pool().get_thread_count() == 2);

    app.reload();
    CHECK(forge::testing::wait_until([&] { return app.call([&] { return app.reloaded; }).get() == 1; }));

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
