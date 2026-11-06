#include <forge/forge.hpp>
#include <forge/testing.hpp>

#include <atomic>
#include <future>
#include <thread>
#include <vector>

using forge::Severity;

TEST_CASE("alarms: raise, repeat, acknowledge, clear") {
    forge::Alarms alarms;
    int changes = 0;
    alarms.on_change([&] { ++changes; });

    CHECK(alarms.raise("tank", Severity::warning, "level 91 %"));
    CHECK_FALSE(alarms.raise("tank", Severity::warning, "level 93 %"));  // repeat: same entry
    auto a = alarms.get("tank");
    REQUIRE(a);
    CHECK(a->count == 2);
    CHECK(a->message == "level 93 %");
    CHECK(a->pending());
    CHECK(alarms.active_count() == 1);
    CHECK(alarms.pending_count() == 1);
    CHECK(alarms.highest_active() == Severity::warning);

    CHECK(alarms.acknowledge("tank", "op"));
    CHECK(alarms.pending_count() == 0);
    CHECK(alarms.get("tank")->acknowledged_by == "op");

    // escalation needs attention again
    CHECK(alarms.raise("tank", Severity::critical, "level 99 %"));
    CHECK(alarms.get("tank")->pending());
    CHECK(alarms.highest_active() == Severity::critical);

    CHECK(alarms.clear("tank"));
    CHECK_FALSE(alarms.clear("tank"));
    CHECK_FALSE(alarms.get("tank")->active);
    CHECK(alarms.active_count() == 0);
    CHECK_FALSE(alarms.acknowledge("nope"));
    CHECK(changes >= 5);
}

TEST_CASE("alarms: flapping condition stays one entry until acknowledged") {
    forge::Alarms alarms;
    alarms.raise("temp", Severity::warning, "hot");
    alarms.clear("temp");
    CHECK_FALSE(alarms.raise("temp", Severity::warning, "hot again"));  // not acknowledged: same entry
    CHECK(alarms.get("temp")->count == 2);
    CHECK(alarms.get("temp")->active);

    alarms.acknowledge("temp");
    alarms.clear("temp");
    CHECK(alarms.raise("temp", Severity::warning, "new occurrence"));  // acknowledged + cleared: new one
    CHECK(alarms.get("temp")->count == 1);
}

TEST_CASE("alarms: set(), ordering, history limit, json") {
    forge::Alarms alarms(2);
    alarms.set("a", true, Severity::info, "a");
    alarms.set("b", true, Severity::error, "b");
    alarms.set("c", true, Severity::warning, "c");
    alarms.acknowledge("b");

    auto list = alarms.list();
    REQUIRE(list.size() == 3);
    CHECK(list[0].id == "c");  // pending first, most severe first
    CHECK(list[1].id == "a");
    CHECK(list[2].id == "b");  // acknowledged active after pending

    for (const char* id : {"a", "b", "c"}) {
        alarms.acknowledge(id);
        alarms.set(id, false, Severity::info, "");
    }
    CHECK(alarms.list().size() == 2);  // history trimmed to 2 cleared entries
    alarms.purge_cleared();
    CHECK(alarms.list().empty());

    alarms.raise("x", Severity::critical, "boom");
    const auto j = alarms.to_json();
    CHECK(j[0]["severity"] == "critical");
    CHECK(j[0]["active"] == true);
    CHECK(j[0]["raised_at"].get<std::int64_t>() > 0);
    CHECK(alarms.summary()["highest"] == "critical");
    CHECK(alarms.acknowledge_all("ops") == 1);

    CHECK(forge::parse_severity("error") == Severity::error);
    CHECK_THROWS_AS(forge::parse_severity("loud"), std::invalid_argument);
    CHECK_THROWS_AS(alarms.raise("", Severity::info, "x"), std::invalid_argument);
}

TEST_CASE("alarms: thread-safe raise/clear from many threads") {
    forge::Alarms alarms;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 500; ++i) {
                const auto id = "worker." + std::to_string(t % 4);
                alarms.set(id, i % 3 != 0, Severity::warning, "load");
                if (i % 50 == 0) alarms.acknowledge_all();
                (void)alarms.to_json();
            }
        });
    }
    for (auto& t : threads) t.join();
    CHECK(alarms.list().size() <= 4);
}

TEST_CASE("application: alarm API and health summary") {
    std::vector<std::string> args{"alarmapp", "--http", "127.0.0.1:0", "-D", "log.console=false"};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());

    forge::Application app({"alarmapp"});
    std::promise<int> port;
    app.hook_start([&](forge::Application& a) { a.post([&] { port.set_value(a.http()->port()); }); });
    std::thread runner([&] { app.run(static_cast<int>(argv.size()), argv.data()); });
    const int p = port.get_future().get();
    forge::http::Client client("http://127.0.0.1:" + std::to_string(p));

    app.alarms().raise("pump.dry", Severity::error, "Pump running dry");  // from a non-main thread
    CHECK(client.get_json("/health")["alarms"]["pending"] == 1);
    auto list = client.get_json("/forge/api/alarms");
    REQUIRE(list.size() == 1);
    CHECK(list[0]["id"] == "pump.dry");

    const auto acked = client.post_json("/forge/api/alarms/pump.dry/ack", forge::json::object());
    CHECK(acked["acknowledged"] == true);
    CHECK(acked["acknowledged_by"].get<std::string>().rfind("web ", 0) == 0);
    CHECK_THROWS_AS(client.post_json("/forge/api/alarms/nope/ack", forge::json::object()), forge::http::HttpError);

    app.alarms().raise("a", Severity::info, "x");
    app.alarms().raise("b", Severity::info, "y");
    CHECK(client.post_json("/forge/api/alarms/ack", forge::json::object())["acknowledged"] == 2);
    CHECK(client.get_json("/health")["alarms"]["pending"] == 0);

    app.stop();
    runner.join();
}
