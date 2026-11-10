// forge::Application's internal services on their own (src/app/services.hpp).
#include "app/main_loop.hpp"
#include "app/services.hpp"
#include "app/signals.hpp"

#include <forge/forge.hpp>
#include <forge/testing.hpp>

#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <future>
#include <thread>
#include <vector>

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

namespace {

/// A MainLoop running on a thread of its own, as Application::run() runs it.
struct RunningLoop {
    RunningLoop() {
        std::promise<void> bound;
        thread = std::thread([&] {
            loop.bind_to_this_thread();
            bound.set_value();
            loop.run();
        });
        bound.get_future().wait();
    }
    ~RunningLoop() {
        loop.stop();
        thread.join();
    }
    forge::detail::MainLoop loop;
    std::thread thread;
};

}  // namespace

TEST_CASE("services: MainLoop runs posted tasks in order, on its own thread") {
    RunningLoop r;
    CHECK_FALSE(r.loop.on_loop());
    std::vector<int> order;
    bool on_loop = false;
    for (int i = 0; i < 100; ++i) REQUIRE(r.loop.post([&order, i] { order.push_back(i); }));
    r.loop.invoke({}, [&] { on_loop = r.loop.on_loop(); });  // returns after everything posted before it
    CHECK(on_loop);
    REQUIRE(order.size() == 100);
    for (int i = 0; i < 100; ++i) CHECK(order[static_cast<std::size_t>(i)] == i);
    CHECK_FALSE(r.loop.post({}));  // an empty task is refused
}

TEST_CASE("services: MainLoop::invoke throws the task's error on the waiting thread") {
    RunningLoop r;
    CHECK_THROWS_WITH_AS(r.loop.invoke({}, [] { throw std::invalid_argument("bad value"); }), "bad value",
                         std::invalid_argument);
    try {
        r.loop.invoke({}, [] { throw forge::Error(409, "taken", json{{"by", "someone"}}); });
        FAIL("no exception");
    } catch (const forge::Error& e) {
        CHECK(e.status() == 409);
        CHECK(e.details()["by"] == "someone");
    }
    int after = 0;
    r.loop.invoke({}, [&] { after = 1; });  // the loop goes on after a task that threw
    CHECK(after == 1);
}

TEST_CASE("services: MainLoop::invoke gives up at its deadline and the task never runs") {
    RunningLoop r;
    std::promise<void> release;
    std::promise<void> entered;
    REQUIRE(r.loop.post([&] {
        entered.set_value();
        release.get_future().wait();  // the loop is stuck here
    }));
    entered.get_future().wait();
    bool ran = false;
    try {
        r.loop.invoke(std::chrono::steady_clock::now() + std::chrono::milliseconds(50), [&] { ran = true; });
        FAIL("no exception");
    } catch (const forge::Error& e) {
        CHECK(e.status() == 503);
    }
    release.set_value();
    r.loop.invoke({}, [] {});  // everything queued before has been handled
    CHECK_FALSE(ran);
}

TEST_CASE("services: MainLoop refuses tasks beyond [loop].max_pending_tasks") {
    forge::detail::MainLoop loop;  // not running: what is posted stays queued
    loop.configure(2000, 200, 3);
    int ran = 0;
    for (int i = 0; i < 3; ++i) CHECK(loop.post([&] { ++ran; }));
    CHECK_FALSE(loop.post([&] { ++ran; }));
    CHECK(loop.queued() == 3);
    loop.discard_pending();
    CHECK(loop.queued() == 0);
    CHECK(ran == 0);
}

TEST_CASE("services: MainLoop::drain_while keeps running tasks while a thread waits for the loop") {
    forge::detail::MainLoop loop;
    loop.bind_to_this_thread();  // this thread is the loop, as in Application's shutdown
    int value = 0;
    // The helper waits for the loop (as a request handler does in invoke()):
    // it only returns because drain_while() keeps running posted tasks.
    loop.drain_while([&] { loop.invoke({}, [&] { value = 42; }); });
    CHECK(value == 42);
}

TEST_CASE("services: MainLoop::age_ms is the time since the last beat") {
    forge::detail::MainLoop loop;
    CHECK(loop.age_ms() == 0);  // no beat yet
    loop.beat();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(loop.age_ms() >= 25);
    loop.track_age(false);  // a manual clock
    CHECK(loop.age_ms() == 0);
}

TEST_CASE("services: SignalWatcher hands a blocked signal to its callback and restores the mask") {
    sigset_t before;
    pthread_sigmask(SIG_SETMASK, nullptr, &before);
    {
        forge::detail::SignalWatcher signals;
        signals.block();  // before the thread exists: it inherits the mask
        std::promise<int> got;
        signals.start([&](int sig) { got.set_value(sig); });
        REQUIRE(::kill(::getpid(), SIGHUP) == 0);
        auto f = got.get_future();
        REQUIRE(f.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        CHECK(f.get() == SIGHUP);
        signals.stop();
        signals.restore();
    }
    sigset_t after;
    pthread_sigmask(SIG_SETMASK, nullptr, &after);
    CHECK(sigismember(&after, SIGHUP) == sigismember(&before, SIGHUP));
    CHECK(sigismember(&after, SIGTERM) == sigismember(&before, SIGTERM));
}
