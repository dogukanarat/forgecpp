#include <forge/events.hpp>
#include <forge/testing.hpp>

#include <atomic>
#include <chrono>
#include <thread>

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
    sub.reset();  // the bus is gone: nothing to remove, nothing to wait for
    CHECK_FALSE(sub.active());
}

TEST_CASE("events: reset() returns once a handler running on another thread is done") {
    forge::EventBus bus;
    std::atomic<bool> entered{false}, may_leave{false}, left{false};
    auto sub = bus.subscribe<Ping>([&](const Ping&) {
        entered = true;
        while (!may_leave) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        left = true;
    });
    std::thread publisher([&] { bus.publish(Ping{1}); });
    while (!entered) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::thread resetter([&] { sub.reset(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(left);  // reset() is waiting for the handler
    may_leave = true;
    resetter.join();
    CHECK(left);  // and returned only after it finished
    publisher.join();
    CHECK(bus.subscriber_count<Ping>() == 0);
}

TEST_CASE("events: handler exceptions do not stop delivery") {
    forge::EventBus bus;
    int calls = 0;
    auto a = bus.subscribe<Ping>([](const Ping&) { throw std::runtime_error("boom"); });
    auto b = bus.subscribe<Ping>([&](const Ping&) { ++calls; });
    bus.publish(Ping{0});
    CHECK(calls == 1);
}
