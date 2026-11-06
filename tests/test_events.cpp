#include <forge/events.hpp>
#include <forge/testing.hpp>

#include <string>

namespace {
struct Ping {
    int value;
};
struct Other {
    std::string text;
};
}  // namespace

TEST_CASE("events: subscribe, publish, unsubscribe") {
    forge::EventBus bus;
    int total = 0;
    std::string text;
    {
        auto a = bus.subscribe<Ping>([&](const Ping& p) { total += p.value; });
        auto b = bus.subscribe<Ping>([&](const Ping& p) { total += p.value * 10; });
        auto c = bus.subscribe<Other>([&](const Other& o) { text = o.text; });
        CHECK(bus.subscriber_count<Ping>() == 2);

        bus.publish(Ping{1});
        bus.publish(Other{"hi"});
        CHECK(total == 11);
        CHECK(text == "hi");

        b.reset();
        bus.publish(Ping{1});
        CHECK(total == 12);
    }
    CHECK(bus.subscriber_count<Ping>() == 0);
    bus.publish(Ping{1});
    CHECK(total == 12);
}

TEST_CASE("events: subscription outliving the bus is safe") {
    forge::EventBus::Subscription sub;
    {
        forge::EventBus bus;
        sub = bus.subscribe<Ping>([](const Ping&) {});
        CHECK(sub.active());
    }
    CHECK_FALSE(sub.active());
    sub.reset();
}

TEST_CASE("events: handler exceptions do not stop delivery") {
    forge::EventBus bus;
    int calls = 0;
    auto a = bus.subscribe<Ping>([](const Ping&) { throw std::runtime_error("boom"); });
    auto b = bus.subscribe<Ping>([&](const Ping&) { ++calls; });
    bus.publish(Ping{0});
    CHECK(calls == 1);
}
