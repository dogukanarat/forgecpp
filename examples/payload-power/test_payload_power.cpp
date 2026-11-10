// The payload-power example on the simulated GPIO chip, with the manual
// clock: the timers move only when the test says so.
#include <forge/forge.hpp>
#include <forge/testing.hpp>

// main.cpp ends with FORGE_MAIN(PayloadPower); the test has its own main().
#undef FORGE_MAIN
#define FORGE_MAIN(AppClass)
#include "main.cpp"

namespace {

using S = PayloadPower::State;
using Runner = forge::testing::AppRunner<PayloadPower>;

Runner start(std::vector<std::string> args = {}) {
    args.insert(args.end(), {"-D", "http.enabled=false", "-D", "payload.chip=sim:test"});
    return Runner(std::move(args), {.manual_time = true});
}

S state(Runner& r) {
    return r->invoke([&] { return r->state(); });
}

// The PGOOD edge reaches the main loop through the worker thread.
bool becomes(Runner& r, S s) {
    return forge::testing::wait_until([&] { return state(r) == s; });
}

}  // namespace

TEST_CASE("payload-power: EN goes high, PGOOD follows, the payload is ready") {
    auto r = start();
    CHECK(state(r) == S::powering);
    CHECK(forge::gpio::sim::level("sim:test", "PAYLOAD_EN"));
    r.advance(300ms);  // the simulated regulator
    CHECK(becomes(r, S::ready));
    CHECK(r->invoke([&] { return r->dashboard()->get("pgood"); }) == true);
}

TEST_CASE("payload-power: no PGOOD: retries, then fails with an alarm and EN low") {
    auto r = start({"-D", "payload.power_on_start=false"});
    r->invoke([&] {
        r->dashboard()->set("sim_answers", false);
        r->dashboard()->press("power_up");
    });
    for (int attempt = 1; attempt <= 3; ++attempt) {
        CHECK(state(r) == S::powering);
        r.advance(2s);  // boot_ms
        if (attempt < 3) {
            CHECK(state(r) == S::retrying);
            CHECK_FALSE(forge::gpio::sim::level("sim:test", "PAYLOAD_EN"));
            r.advance(1s);
        }
    }
    CHECK(state(r) == S::failed);
    CHECK_FALSE(forge::gpio::sim::level("sim:test", "PAYLOAD_EN"));
    CHECK(r->invoke([&] { return r->alarms().highest_active(); }) == forge::Severity::error);
}

TEST_CASE("payload-power: a brown-out while ready powers the payload up again") {
    auto r = start();
    r.advance(300ms);
    REQUIRE(becomes(r, S::ready));
    r->invoke([&] { r->dashboard()->press("brown_out"); });
    CHECK(becomes(r, S::powering));  // PGOOD fell: try again
    CHECK(r->invoke([&] { return r->alarms().highest_active(); }) == forge::Severity::warning);
    r.advance(300ms);
    CHECK(becomes(r, S::ready));
    CHECK_FALSE(r->invoke([&] { return r->alarms().highest_active(); }).has_value());  // cleared when ready
}

TEST_CASE("payload-power: power off from the panel; the lines are released at the end") {
    {
        auto r = start();
        r->invoke([&] { r->dashboard()->press("power_off"); });
        CHECK(state(r) == S::off);
        CHECK_FALSE(forge::gpio::sim::level("sim:test", "PAYLOAD_EN"));
        CHECK(forge::gpio::sim::requested("sim:test", "PAYLOAD_PGOOD"));
    }
    CHECK_FALSE(forge::gpio::sim::requested("sim:test", "PAYLOAD_EN"));
    CHECK_FALSE(forge::gpio::sim::requested("sim:test", "PAYLOAD_PGOOD"));
}
