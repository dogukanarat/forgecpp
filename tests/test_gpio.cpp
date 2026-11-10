// forge::gpio on the simulated chip; the kernel path only for its errors
// (a machine without /dev/gpiochip*).
#include <forge/gpio.hpp>
#include <forge/testing.hpp>

#include <chrono>
#include <stdexcept>
#include <thread>

namespace gpio = forge::gpio;
using namespace std::chrono_literals;

TEST_CASE("gpio: an output drives its line, active low inverts it") {
    {
        gpio::Output en("sim:t1", "EN", {.initial = true});
        CHECK(gpio::sim::requested("sim:t1", "EN"));
        CHECK(gpio::sim::level("sim:t1", "EN"));
        en.set(false);
        CHECK_FALSE(gpio::sim::level("sim:t1", "EN"));
        CHECK_FALSE(en.get());

        gpio::Output reset_n("sim:t1", "RESET_N", {.initial = false, .active_low = true});
        CHECK(gpio::sim::level("sim:t1", "RESET_N"));  // inactive = high
        reset_n.set(true);
        CHECK_FALSE(gpio::sim::level("sim:t1", "RESET_N"));
        CHECK(reset_n.get());
    }
    CHECK_FALSE(gpio::sim::requested("sim:t1", "EN"));  // released with the object
}

TEST_CASE("gpio: a line is held by one request at a time") {
    gpio::Output a("sim:t2", "LED");
    CHECK_THROWS_WITH_AS(gpio::Output("sim:t2", "LED"), doctest::Contains("in use"), std::runtime_error);
    CHECK_THROWS_AS(gpio::Input("sim:t2", "LED"), std::runtime_error);
    gpio::Output moved = std::move(a);  // the request moves with the object
    CHECK(gpio::sim::requested("sim:t2", "LED"));
}

TEST_CASE("gpio: an input reads its line and reports the edges it asked for") {
    gpio::sim::drive("sim:t3", "PGOOD", false);
    gpio::Input pgood("sim:t3", "PGOOD", {.edge = gpio::Edge::both});
    CHECK_FALSE(pgood.get());
    CHECK_FALSE(pgood.wait(10ms));  // nothing happened

    gpio::sim::drive("sim:t3", "PGOOD", true);
    auto e = pgood.wait(1s);
    REQUIRE(e);
    CHECK(e->rising);
    CHECK(pgood.get());
    gpio::sim::drive("sim:t3", "PGOOD", true);  // no change, no edge
    gpio::sim::drive("sim:t3", "PGOOD", false);
    e = pgood.wait(1s);
    REQUIRE(e);
    CHECK_FALSE(e->rising);

    // Only rising edges, active low: the line going low is the rising edge.
    gpio::sim::drive("sim:t3", "BUTTON_N", true);
    gpio::Input button("sim:t3", "BUTTON_N", {.active_low = true, .edge = gpio::Edge::rising});
    CHECK_FALSE(button.get());
    gpio::sim::drive("sim:t3", "BUTTON_N", false);  // pressed
    gpio::sim::drive("sim:t3", "BUTTON_N", true);   // released: not asked for
    e = button.wait(1s);
    REQUIRE(e);
    CHECK(e->rising);
    CHECK_FALSE(button.wait(10ms));
}

TEST_CASE("gpio: wait() returns when an edge comes from another thread") {
    gpio::Input irq("sim:t4", "IRQ", {.edge = gpio::Edge::rising});
    std::thread device([] {
        std::this_thread::sleep_for(20ms);
        gpio::sim::drive("sim:t4", "IRQ", true);
    });
    const auto e = irq.wait(5s);
    device.join();
    REQUIRE(e);
    CHECK(e->seqno == 1);
}

TEST_CASE("gpio: a bias sets the level of an unconnected simulated input") {
    gpio::Input up("sim:t5", "UP", {.bias = gpio::Bias::pull_up});
    CHECK(up.get());
}

TEST_CASE("gpio: a chip or a line that is not there is named in the error") {
    CHECK_THROWS_WITH_AS(gpio::Output("/dev/gpiochip99", "EN"), doctest::Contains("/dev/gpiochip99"),
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(gpio::Input("no-such-chip", "1"), doctest::Contains("no chip no-such-chip"),
                         std::runtime_error);
    CHECK_THROWS_AS(gpio::sim::drive("gpiochip0", "X", true), std::invalid_argument);
}
