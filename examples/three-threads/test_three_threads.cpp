// The three-threads example, driven through its controls as the panel does.
#include <forge/forge.hpp>
#include <forge/testing.hpp>

// main.cpp ends with FORGE_MAIN(ThreeThreads); the test has its own main().
#undef FORGE_MAIN
#define FORGE_MAIN(AppClass)
#include "main.cpp"

namespace {

double control(forge::testing::AppRunner<ThreeThreads>& r, const std::string& id) {
    return r->invoke([&] { return r->dashboard()->get(id).get<double>(); });
}

}  // namespace

TEST_CASE("three-threads: samples go through the three threads to the main loop") {
    forge::testing::AppRunner<ThreeThreads> r({"-D", "http.enabled=false", "-D", "demo.rate_hz=1000"});
    // acquire -> process -> main loop (the mean) and -> downlink (once a second)
    CHECK(forge::testing::wait_until([&] { return control(r, "blocks") >= 20; }));
    CHECK(control(r, "mean") != 0.0);
    CHECK(forge::testing::wait_until([&] { return control(r, "sent") > 0; }));
    CHECK(control(r, "acquired") >= 10 * control(r, "blocks"));
}

TEST_CASE("three-threads: the rate control reaches the acquire thread") {
    forge::testing::AppRunner<ThreeThreads> r({"-D", "http.enabled=false", "-D", "demo.rate_hz=10"});
    r->invoke([&] { r->dashboard()->set("rate_hz", 2000); });
    // At 10 Hz, 500 samples would take 50 s.
    CHECK(forge::testing::wait_until([&] { return control(r, "acquired") >= 500; }));
}

TEST_CASE("three-threads: a thread that fails shows in health and alarms") {
    forge::testing::AppRunner<ThreeThreads> r({"-D", "http.enabled=false"});
    r->invoke([&] { r->dashboard()->set("fail", true); });
    CHECK(forge::testing::wait_until(
        [&] { return r->invoke([&] { return r->alarms().highest_active(); }) == forge::Severity::error; }));
    CHECK(r->health().overall() != forge::HealthState::ok);
    CHECK(r.stop() == 0);  // the other two threads still stop cleanly
}
