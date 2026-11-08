// The tutorial's tests (docs/tutorials/04-testing.md).
#include "pump.hpp"

#include <forge/testing.hpp>

using namespace std::chrono_literals;

TEST_CASE("pump: controls through the command link, like forgectl") {
    forge::testing::AppRunner<Pump> r({"-D", "forge.control=tcp://127.0.0.1:*", "-D", "forge.data=tcp://127.0.0.1:*",
                                       "-D", "http.enabled=false"});
    forge::Remote pump(r->endpoints().at("control"));
    CHECK(pump.set("speed_rpm", 1500) == 1500);
    pump.press("start");
    CHECK(pump.get("running") == true);
    CHECK(pump.call("flush", {{"seconds", 2}})["flushing_s"] == 2);
    try {
        pump.set("speed_rpm", 9999);
        FAIL("no error");
    } catch (const forge::Error& e) {
        CHECK(e.status() == 400);  // outside 0..3000
    }
}

TEST_CASE("pump: overpressure alarm after 3 s, with hysteresis (manual time: no sleeping)") {
    forge::testing::AppRunner<Pump> r({"-D", "http.enabled=false"}, {.manual_time = true});
    r->invoke([&] {
        r->speed_rpm = 2500;  // -> 10 bar
        r->running = true;
    });
    r.advance(2s);  // pressure rises above 8 bar after ~0.7 s
    CHECK_FALSE(r->alarms().active("pump.overpressure"));
    r.advance(2s);
    CHECK(r->alarms().active("pump.overpressure"));

    r->invoke([&] { r->running = false; });
    r.advance(5s);
    CHECK_FALSE(r->alarms().active("pump.overpressure"));
    CHECK(r->invoke([&] { return r->pressure_bar; }) < 1.0);
}
