// forge::Application's internal services on their own (src/app/services.hpp).
#include "app/services.hpp"

#include <forge/forge.hpp>
#include <forge/testing.hpp>

#include <cstdlib>

using forge::json;

TEST_CASE("services: StateStore restores persistent controls and saves only when they changed") {
    forge::testing::TempDir dir;
    const auto file = (dir / "state.json").string();
    auto logger = forge::log::get("test");
    double gain = 1, volatile_value = 2;
    forge::web::Dashboard d("t");
    auto& g = d.group("G");
    g.number("gain", "Gain", gain).persist();
    g.number("other", "Other", volatile_value);

    forge::detail::StateStore store;
    store.restore(d, file, logger);  // no file yet: nothing happens
    store.save(d, logger);           // not dirty: nothing written
    CHECK_FALSE(std::filesystem::exists(file));
    d.set("gain", 7);
    store.mark_dirty();
    store.save(d, logger);
    CHECK(forge::load_json(file) == json{{"gain", 7}});  // only persistent controls

    d.set("gain", 1);
    forge::detail::StateStore again;
    again.restore(d, file, logger);
    CHECK(gain == 7);
    again.save(d, logger, true);  // forced (shutdown)
    CHECK(forge::load_json(file)["gain"] == 7);

    forge::detail::StateStore off;
    off.restore(d, "", logger);
    off.mark_dirty();
    off.save(d, logger, true);  // no file configured: nothing to do

    // With a pool the file is written off the calling thread (the main loop).
    forge::ThreadPool pool(1);
    forge::detail::StateStore async;
    async.restore(d, file, logger);
    d.set("gain", 9);
    async.mark_dirty();
    async.save(d, logger, false, &pool);
    pool.wait();
    CHECK(forge::load_json(file)["gain"] == 9);
    async.save(d, logger, false, &pool);  // not dirty: nothing queued
    pool.wait();

    // A forced save (shutdown) while writes of the pool are on their way: the
    // file ends with the newest values, whichever thread wrote last.
    for (int round = 0; round < 50; ++round) {
        d.set("gain", 100 + round);
        async.mark_dirty();
        async.save(d, logger, false, &pool);  // on a pool thread
        d.set("gain", 1000 + round);
        async.save(d, logger, true);  // here, at once
        pool.wait();
        REQUIRE(forge::load_json(file)["gain"] == 1000 + round);
    }
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(file).parent_path())) {
        CHECK(entry.path().filename().string().find(".tmp.") == std::string::npos);  // no temporary file left
    }
}

TEST_CASE("services: LayoutStore keeps page layouts in a file, or only in memory with \"-\"") {
    forge::testing::TempDir dir;
    const auto file = (dir / "sub" / "layout.json").string();
    auto logger = forge::log::get("test");
    forge::detail::LayoutStore s;
    s.load(file, "app", logger);
    CHECK(s.get("panel").is_null());
    CHECK(s.put("panel", json{{"items", json::array({1})}}));
    CHECK(forge::load_json(file)["panel"]["items"][0] == 1);
    forge::detail::LayoutStore reloaded;
    reloaded.load(file, "app", logger);
    CHECK(reloaded.get("panel")["items"][0] == 1);
    reloaded.put("panel", json{{"items", json::array({2})}}, "vera");
    forge::detail::LayoutStore again;
    again.load(file, "app", logger);
    CHECK(again.get("panel", "vera")["items"][0] == 2);  // users' layouts are kept in the file too
    CHECK(again.get("panel")["items"][0] == 1);
    reloaded.remove("panel");
    CHECK_FALSE(forge::load_json(file).contains("panel"));

    forge::detail::LayoutStore memory;
    memory.load("-", "app", logger);
    CHECK(memory.file().empty());
    CHECK_FALSE(memory.put("x", json::object()));
    CHECK(memory.get("x")["scope"] == "device");
    // Users' layouts next to the device's.
    CHECK(memory.put("x", json{{"items", 1}}, "vera") == false);
    CHECK(memory.get("x", "vera")["items"] == 1);
    CHECK(memory.get("x", "vera")["scope"] == "user");
    CHECK(memory.get("x", "ada")["scope"] == "device");
    memory.remove("x", "vera");
    CHECK(memory.get("x", "vera")["scope"] == "device");

    CHECK(forge::detail::LayoutStore::valid_page("radio-1.main_page"));
    CHECK_FALSE(forge::detail::LayoutStore::valid_page("../etc"));
    CHECK_FALSE(forge::detail::LayoutStore::valid_page(""));

    ::setenv("STATE_DIRECTORY", "/var/lib/app:/var/lib/other", 1);
    CHECK(forge::detail::LayoutStore::default_file("app") == "/var/lib/app/web-layout.json");
    ::unsetenv("STATE_DIRECTORY");
}

TEST_CASE("services: Watchdog stops pinging once a critical component failed longer than the grace time") {
    auto logger = forge::log::get("test");
    forge::Health health;
    forge::detail::Watchdog wd;
    CHECK(wd.tick(health, 0.05, logger));
    health.set("radio", forge::HealthState::failed, "lost", true);
    CHECK(wd.tick(health, 0.05, logger));  // within the grace time
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK_FALSE(wd.tick(health, 0.05, logger));
    CHECK(wd.paused());
    CHECK(wd.tick(health, 0, logger));  // grace 0: never stops
    health.set("radio", forge::HealthState::ok);
    CHECK(wd.tick(health, 0.05, logger));
    CHECK_FALSE(wd.paused());
}
