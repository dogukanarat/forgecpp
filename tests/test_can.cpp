// forge::can without a CAN interface (this machine's kernel may have no
// AF_CAN): the frames and the ISO-TP link run over SOCK_SEQPACKET socket
// pairs, which keep message boundaries as CAN_RAW and CAN_ISOTP sockets do.
#include <forge/can.hpp>
#include <forge/forge.hpp>
#include <forge/testing.hpp>
#include <forge/tmtc.hpp>

#include "forge_test/commands.forge.hpp"

#include <linux/can.h>
#include <sys/socket.h>
#include <unistd.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace can = forge::can;
namespace pb = forge::pb;
namespace serial = forge::serial;
namespace tmtc = forge::tmtc;
using namespace std::chrono_literals;

namespace {

struct Pair {
    int a = -1, b = -1;
    Pair() {
        int sv[2];
        REQUIRE(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
        a = sv[0];
        b = sv[1];
    }
};

constexpr std::uint16_t kEcho = 0x0101;

}  // namespace

TEST_CASE("can: frames go out and come back as they were") {
    Pair p;
    can::Socket tx(p.a), rx(p.b);
    REQUIRE(tx.send(can::Frame::make(0x123, {1, 2, 3})).ok());
    can::Frame f;
    REQUIRE(rx.receive(f, 1s).ok());
    CHECK(f.id == 0x123);
    CHECK_FALSE(f.extended);
    CHECK_FALSE(f.fd);
    CHECK(std::vector<std::uint8_t>(f.bytes().begin(), f.bytes().end()) == std::vector<std::uint8_t>{1, 2, 3});

    auto ext = can::Frame::make(0x18FF50E5, {0xAA}, true);
    REQUIRE(tx.send(ext).ok());
    REQUIRE(rx.receive(f, 1s).ok());
    CHECK(f.extended);
    CHECK(f.id == 0x18FF50E5);

    CHECK(rx.receive(f, 10ms).errno_code() == ETIMEDOUT);  // nothing more
}

TEST_CASE("can: a frame of the wrong size is refused before it is sent") {
    Pair p;
    can::Socket classic(p.a), fd_socket(p.b, true);
    CHECK_THROWS_AS(can::Frame::make(0x1, {1, 2, 3, 4, 5, 6, 7, 8, 9}), std::invalid_argument);

    can::Frame big;
    big.id = 0x10;
    big.fd = true;
    big.size = 12;
    CHECK(classic.send(big).errno_code() == EINVAL);    // the socket takes no FD frames
    REQUIRE(fd_socket.send(big).ok());                   // 12 is a CAN FD size
    can::Frame f;
    REQUIRE(classic.receive(f, 1s).ok());
    CHECK(f.fd);
    CHECK(f.size == 12);
    big.size = 13;
    CHECK(fd_socket.send(big).errno_code() == EINVAL);  // 13 is not
}

TEST_CASE("can: c2 commands over an ISO-TP style link, one framed piece per message") {
    tmtc::Registry registry;
    registry.add<forge_test_echo_request, forge_test_echo_reply>(
        kEcho, "echo", [](const forge_test_echo_request& req, forge_test_echo_reply& rsp) {
        rsp.token = req.token;
        return pb::assign(rsp.text, pb::view(req.text));
    });
    Pair p;
    serial::LinkOptions device;  // the program, as Messaging opens [can]
    device.fd = p.a;
    device.label = "can0 0x701/0x700";
    device.frame_per_write = true;
    device.framing.frame_size = 16;  // several pieces for each message
    device.on_request = [&](std::span<const std::uint8_t> f, const auto& send) {
        tmtc::Dispatcher(registry).handle(f, send);
    };
    serial::Link program(device);
    CHECK(program.device() == "can0 0x701/0x700");

    // The peer reads the messages raw first: each one is one whole frame.
    serial::Framing framing({.frame_size = 16});
    std::vector<std::uint8_t> wire;
    forge_test_echo_request req{};
    req.token = 7;
    REQUIRE(pb::assign(req.text, "fifteen chars!!").ok());
    std::array<std::uint8_t, 256> payload{};
    std::size_t n = 0;
    REQUIRE(pb::encode(req, payload, n).ok());
    std::array<std::uint8_t, 300> frame{};
    std::size_t size = 0;
    REQUIRE(
        forge::c2::encode_message(forge::c2::make_header(forge::c2::flag::req, kEcho, 1, static_cast<std::uint16_t>(n)),
                                  std::span(payload).first(n), frame, size)
            .ok());
    REQUIRE(framing.encode(std::span(frame).first(size), wire).ok());
    // Send it the way an ISO-TP peer would: one framed piece per message.
    std::size_t pieces_out = 0;
    for (std::size_t i = 0; i < wire.size();) {
        auto end = std::find(wire.begin() + static_cast<long>(i) + 1, wire.end(), std::uint8_t{0});
        const auto len = static_cast<std::size_t>(end - wire.begin()) - i + 1;
        REQUIRE(::send(p.b, wire.data() + i, len, 0) == static_cast<ssize_t>(len));
        i += len;
        ++pieces_out;
    }
    CHECK(pieces_out > 1);

    std::vector<std::uint8_t> answer;
    std::size_t pieces_in = 0;
    serial::Framing rx({.frame_size = 16});
    bool done = false;
    while (!done) {
        std::array<std::uint8_t, 512> buf{};
        const auto got = ::recv(p.b, buf.data(), buf.size(), 0);
        REQUIRE(got > 0);
        ++pieces_in;
        CHECK(buf[0] == 0);                                    // each message starts a frame
        CHECK(buf[static_cast<std::size_t>(got) - 1] == 0);   // and ends it
        CHECK(static_cast<std::size_t>(got) <= 16);
        rx.receive(std::span(buf).first(static_cast<std::size_t>(got)), [&](std::span<const std::uint8_t> m) {
            answer.assign(m.begin(), m.end());
            done = true;
        });
    }
    CHECK(pieces_in > 1);
    forge::c2::Message m;
    REQUIRE(forge::c2::decode_message(answer, m).ok());
    forge_test_echo_reply rsp{};
    REQUIRE(pb::decode(m.payload, rsp).ok());
    CHECK(rsp.token == 7);
    CHECK(pb::view(rsp.text) == "fifteen chars!!");
}

TEST_CASE("can: two links talk over a datagram pair") {
    tmtc::Registry registry;
    registry.add<forge_test_echo_request, forge_test_echo_reply>(
        kEcho, "echo", [](const forge_test_echo_request& req, forge_test_echo_reply& rsp) {
        rsp.token = req.token + 1;
        return forge::Status();
    });
    Pair p;
    serial::LinkOptions a;
    a.fd = p.a;
    a.frame_per_write = true;
    a.on_request = [&](std::span<const std::uint8_t> f, const auto& send) {
        tmtc::Dispatcher(registry).handle(f, send);
    };
    serial::Link device(a);
    serial::LinkOptions b;
    b.fd = p.b;
    b.frame_per_write = true;
    serial::Link peer(b);
    forge_test_echo_reply rsp{};
    REQUIRE(peer.call(kEcho, forge_test_echo_request{41}, rsp).ok());
    CHECK(rsp.token == 42);
}

TEST_CASE("can: an interface that is not there is named in the error") {
    CHECK_THROWS_WITH_AS(can::Socket("nocan9"), doctest::Contains("no interface nocan9"), std::runtime_error);
    CHECK_THROWS_WITH_AS(can::open_isotp({.interface = "nocan9"}), doctest::Contains("nocan9"), std::runtime_error);
    CHECK(can::describe({.interface = "can0", .tx_id = 0x701, .rx_id = 0x700}) == "can0 0x701/0x700");
}

TEST_CASE("can: [can] is a link of the application; a missing interface is a health failure, not a stop") {
    forge::testing::AppRunner<forge::Application> r(
        {"-D", "http.enabled=false", "-D", "can.interface=nocan9", "-D", "can.tx_id=0x7E8"},
        std::make_unique<forge::Application>(forge::AppInfo{"can-test", "1.0.0", "CAN"}));
    CHECK(r->endpoints().at("can") == "nocan9 0x7E8/0x700");
    const auto report = r->health().snapshot();
    bool failed = false;
    for (const auto& c : report.components) {
        if (c.name == "can") failed = c.state == forge::HealthState::failed;
    }
    CHECK(failed);
    CHECK_FALSE(r->stopping());
}
