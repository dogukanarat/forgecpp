# 5. Testing

`forge_add_test(pump_tests SOURCES tests/test_pump.cpp ...)` builds a doctest
executable registered with CTest; `#include <forge/testing.hpp>` brings
doctest, `TempDir`, `wait_until` and `AppRunner`.

**The application as a client sees it.** `AppRunner` runs it on a background
thread with an empty configuration; use ephemeral ports so tests can run in
parallel:

```cpp
TEST_CASE("pump: controls through the command link, like forgectl") {
    forge::testing::AppRunner<Pump> r({"-D", "forge.control=tcp://127.0.0.1:*", "-D", "http.enabled=false"});
    forge::Remote pump(r->endpoints().at("control"));
    CHECK(pump.set("speed_rpm", 1500) == 1500);
    pump.press("start");
    CHECK(pump.get("running") == true);
    CHECK(pump.call("flush", {{"seconds", 2}})["flushing_s"] == 2);
}
```

**Time you control.** With `manual_time` timers only fire when the test
advances the clock; each runs at the time it was due, in order. No sleeping,
no flaky margins, a day in seconds:

```cpp
TEST_CASE("pump: overpressure alarm after 3 s, with hysteresis") {
    forge::testing::AppRunner<Pump> r({"-D", "http.enabled=false"}, {.manual_time = true});
    r->invoke([&] { r->speed_rpm = 2500; r->running = true; });   // state: on the loop
    r.advance(2s);
    CHECK_FALSE(r->alarms().active("pump.overpressure"));
    r.advance(2s);
    CHECK(r->alarms().active("pump.overpressure"));
}
```

Read the application's state with `r->invoke(...)` (it belongs to the main
loop; the dashboard throws if you don't). Sanitizers:
`install.sh --sanitize address,undefined` or `thread`; set
`FORGE_TEST_TIMEOUT_SCALE=4` for slow builds. Next: [streams](06-streams.md).
