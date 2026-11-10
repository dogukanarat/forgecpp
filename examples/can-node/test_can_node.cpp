// The can-node example with the bus replaced by a socket pair: the test is
// the sensor node on the other end.
#include <forge/forge.hpp>
#include <forge/testing.hpp>

#include <sys/socket.h>

#include <stdexcept>
#include <utility>

// main.cpp ends with FORGE_MAIN(CanNode); the test has its own main().
#undef FORGE_MAIN
#define FORGE_MAIN(AppClass)
#include "main.cpp"

namespace {

class TestNode : public CanNode {
public:
    int fd = -1;  // the node's end of the "bus"; -1: no bus
    forge::can::Socket open_bus() override {
        if (fd < 0) throw std::runtime_error("no such interface");
        return forge::can::Socket(std::exchange(fd, -1));
    }
};

struct Bus {
    int node = -1, sensor = -1;
    Bus() {
        int sv[2];
        REQUIRE(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
        node = sv[0];
        sensor = sv[1];
    }
};

forge::can::Frame temperature_frame(double c) {
    const auto raw = static_cast<std::int16_t>(c * 100);
    return forge::can::Frame::make(0x185, {static_cast<std::uint8_t>(raw), static_cast<std::uint8_t>(raw >> 8)});
}

}  // namespace

TEST_CASE("can-node: frames encode and decode as documented") {
    const auto hb = node::heartbeat(5, 1, 0x01020304);
    CHECK(hb.id == 0x705);
    CHECK(hb.size == 5);
    CHECK(hb.data[0] == 1);
    CHECK(hb.data[1] == 0x04);  // little endian
    CHECK(node::temperature(temperature_frame(-12.5)) == -12.5);
    CHECK_FALSE(node::temperature(forge::can::Frame::make(0x185, {1})));
}

TEST_CASE("can-node: heartbeats go out, temperatures come in, an alarm above the limit") {
    Bus bus;
    auto app = std::make_unique<TestNode>();
    app->fd = bus.node;
    forge::testing::AppRunner<TestNode> r({"-D", "http.enabled=false", "-D", "node.max_temp_c=50"}, std::move(app));
    forge::can::Socket sensor(bus.sensor);

    forge::can::Frame f;
    REQUIRE(sensor.receive(f, 3s).ok());  // the first heartbeat
    CHECK(f.id == 0x705);
    CHECK(f.data[0] == 0);  // starting: no temperature yet

    REQUIRE(sensor.send(temperature_frame(21.5)).ok());
    CHECK(forge::testing::wait_until(
        [&] { return r->invoke([&] { return r->dashboard()->get("temperature"); }) == 21.5; }));
    CHECK_FALSE(r->invoke([&] { return r->alarms().highest_active(); }).has_value());

    REQUIRE(sensor.send(temperature_frame(55.25)).ok());
    CHECK(forge::testing::wait_until(
        [&] { return r->invoke([&] { return r->alarms().highest_active(); }) == forge::Severity::warning; }));
    // The next heartbeat says "warning".
    CHECK(forge::testing::wait_until([&] { return sensor.receive(f, 100ms).ok() && f.data[0] == 2; }));
}

TEST_CASE("can-node: without a bus the node runs and reports it under health") {
    forge::testing::AppRunner<TestNode> r({"-D", "http.enabled=false"});
    CHECK(forge::testing::wait_until([&] {
        for (const auto& c : r->health().snapshot().components) {
            if (c.name == "bus") return c.state == forge::HealthState::failed && c.message == "no such interface";
        }
        return false;
    }));
    CHECK_FALSE(r->stopping());
}
