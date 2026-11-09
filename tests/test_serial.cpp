#include <forge/serial.hpp>
#include <forge/testing.hpp>
#include <forge/tmtc.hpp>

#include "forge_test/commands.forge.hpp"

#include <fcntl.h>
#include <pty.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <random>
#include <string>
#include <vector>

namespace c2 = forge::c2;
namespace pb = forge::pb;
namespace serial = forge::serial;
namespace tmtc = forge::tmtc;
using forge::Status;
using Bytes = std::vector<std::uint8_t>;

namespace {

constexpr std::uint16_t kEcho = 0x0101;
constexpr std::uint16_t kCount = 0x0102;
constexpr std::uint16_t kLevel = 0x0181;

Bytes bytes(std::string_view s) { return {s.begin(), s.end()}; }

std::vector<Bytes> receive_all(serial::Framing& f, const Bytes& wire) {
    std::vector<Bytes> out;
    f.receive(wire, [&](std::span<const std::uint8_t> m) { out.emplace_back(m.begin(), m.end()); });
    return out;
}

// Both ends of a pseudo terminal: `device` is the path of one end, `fd` the other.
struct Pty {
    int fd = -1;
    std::string device;
    Pty() {
        int other = -1;
        char name[128] = {};
        REQUIRE(::openpty(&fd, &other, name, nullptr, nullptr) == 0);
        device = name;
        ::close(other);  // the link opens it by name, like a real tty
    }
};

tmtc::Registry commands() {
    tmtc::Registry r;
    r.add<forge_test_echo_request, forge_test_echo_reply>(
        kEcho, "echo", [](const forge_test_echo_request& req, forge_test_echo_reply& rsp) {
        rsp.token = req.token;
        return pb::assign(rsp.text, pb::view(req.text));
    });
    r.add_parts<forge_test_count_request, forge_test_count_reply>(
        kCount, "count", [](const forge_test_count_request& req, forge_test_count_reply& rsp, std::uint32_t& more) {
        rsp.value = more + 1;
        more = rsp.value < req.to ? rsp.value : 0;
        return Status();
    });
    return r;
}

}  // namespace

TEST_CASE("serial: CRC16-CCITT") {
    CHECK(serial::crc16(bytes("123456789")) == 0x29B1);  // the check value of CRC-16/CCITT-FALSE
    CHECK(serial::crc16({}) == 0xFFFF);
}

TEST_CASE("serial: a frame has no zero inside and decodes back") {
    std::mt19937 rng(7);
    for (std::size_t len : {0, 1, 2, 253, 254, 255, 508, 600}) {
        for (int round = 0; round < 20; ++round) {
            Bytes payload(len);
            for (auto& b : payload) b = static_cast<std::uint8_t>(round % 2 == 0 ? rng() : rng() % 3);  // many zeros
            Bytes wire;
            serial::encode_frame(payload, wire);
            REQUIRE(wire.size() >= 2);
            CHECK(wire.front() == 0);
            CHECK(wire.back() == 0);
            CHECK(std::count(wire.begin() + 1, wire.end() - 1, 0) == 0);
            CHECK(wire.size() <= payload.size() + 2 + (payload.size() + 2) / 254 + 3);
            Bytes back;
            REQUIRE(serial::decode_frame(std::span(wire).subspan(1, wire.size() - 2), back).ok());
            CHECK(back == payload);
        }
    }
}

TEST_CASE("serial: bad frames are refused with the reason") {
    Bytes wire;
    serial::encode_frame(bytes("hello"), wire);
    Bytes inner(wire.begin() + 1, wire.end() - 1), out;
    auto corrupt = inner;
    corrupt[2] ^= 0x01;
    CHECK(serial::decode_frame(corrupt, out).errno_code() == EILSEQ);
    CHECK(serial::decode_frame(Bytes{0x01}, out).errno_code() == EMSGSIZE);
    CHECK(serial::decode_frame(Bytes{0x05, 1, 2}, out).errno_code() == EOVERFLOW);
    CHECK(serial::decode_frame(Bytes{0x02, 1, 0, 3}, out).errno_code() == EBADMSG);
    CHECK(serial::decode_frame(corrupt, out).module() == forge::module_id::serial);
}

TEST_CASE("serial: a long message goes in fragments and comes back whole") {
    serial::Framing tx({.frame_size = 32, .max_message = 4096});
    serial::Framing rx({.frame_size = 32, .max_message = 4096});
    CHECK(tx.fragment_payload() == 32 - 3 - 5);
    Bytes message(1000);
    for (std::size_t i = 0; i < message.size(); ++i) message[i] = static_cast<std::uint8_t>(i * 7);
    Bytes wire;
    REQUIRE(tx.encode(message, wire).ok());
    CHECK(tx.stats().fragments_sent == (1000 + 23) / 24);
    // Every frame fits the peer's buffer.
    std::size_t start = 0;
    for (std::size_t i = 1; i < wire.size(); ++i) {
        if (wire[i] == 0 && wire[i - 1] != 0) {
            CHECK(i - start + 1 <= 32);
            start = i + 1;
        }
    }
    // Byte by byte, as a UART gives them.
    std::vector<Bytes> got;
    for (const auto b : wire) {
        rx.receive(std::span(&b, 1), [&](std::span<const std::uint8_t> m) { got.emplace_back(m.begin(), m.end()); });
    }
    REQUIRE(got.size() == 1);
    CHECK(got[0] == message);
    CHECK(rx.stats().messages_received == 1);
}

TEST_CASE("serial: the receiver finds the next message after noise and losses") {
    serial::Framing tx({.frame_size = 24});
    serial::Framing rx({.frame_size = 24});
    const auto a = bytes("the first message, in several fragments");
    const auto b = bytes("the second one");
    Bytes wa, wb;
    REQUIRE(tx.encode(a, wa).ok());
    REQUIRE(tx.encode(b, wb).ok());

    SUBCASE("noise before the first frame") {
        Bytes wire = {0x41, 0x42, 0x13};
        wire.insert(wire.end(), wa.begin(), wa.end());
        CHECK(receive_all(rx, wire) == std::vector<Bytes>{a});
    }
    SUBCASE("a corrupted fragment drops its message, not the next one") {
        Bytes wire = wa;
        wire[wire.size() / 2] ^= 0x20;
        wire.insert(wire.end(), wb.begin(), wb.end());
        CHECK(receive_all(rx, wire) == std::vector<Bytes>{b});
        CHECK(rx.stats().frames_dropped == 1);
        CHECK(rx.stats().messages_dropped + rx.stats().fragments_orphaned >= 1);
    }
    SUBCASE("a lost first fragment: the rest are orphans") {
        const auto end_of_first = std::find(wa.begin() + 1, wa.end(), 0) + 1;
        Bytes wire(end_of_first, wa.end());
        wire.insert(wire.end(), wb.begin(), wb.end());
        CHECK(receive_all(rx, wire) == std::vector<Bytes>{b});
        CHECK(rx.stats().fragments_orphaned > 0);
    }
    SUBCASE("too long and empty messages are not sent") {
        Bytes wire;
        CHECK(tx.encode({}, wire).errno_code() == EINVAL);
        CHECK(tx.encode(Bytes(20000), wire).errno_code() == EMSGSIZE);
        CHECK(wire.empty());
    }
}

TEST_CASE("serial: commands both ways on a pty") {
    Pty pty;
    const auto registry = commands();
    std::atomic<int> pushes{0};
    serial::LinkOptions device_side;  // the program
    device_side.device = pty.device;
    device_side.baud = 115200;
    device_side.framing.frame_size = 16;  // several fragments for each message
    device_side.on_request = [&](std::span<const std::uint8_t> f, const auto& send) {
        tmtc::Dispatcher(registry).handle(f, send);
    };
    serial::Link program(device_side);

    serial::LinkOptions peer_side;  // e.g. a microcontroller
    peer_side.fd = pty.fd;
    peer_side.framing.frame_size = 16;
    peer_side.timeout = std::chrono::milliseconds(2000);
    peer_side.on_push = [&](const c2::Message& m) {
        if (m.header.cmd_id == kLevel) ++pushes;
    };
    serial::Link peer(peer_side);

    forge_test_echo_request req{};
    req.token = 42;
    REQUIRE(pb::assign(req.text, "fifteen chars!!").ok());
    forge_test_echo_reply rsp{};
    REQUIRE(peer.call(kEcho, req, rsp).ok());
    CHECK(rsp.token == 42);
    CHECK(pb::view(rsp.text) == "fifteen chars!!");

    std::vector<std::uint32_t> values;
    REQUIRE(peer.call_parts<forge_test_count_request, forge_test_count_reply>(kCount, {5},
                                                                              [&](const forge_test_count_reply& r) {
        values.push_back(r.value);
    }).ok());
    CHECK(values == std::vector<std::uint32_t>{1, 2, 3, 4, 5});

    // No such command: the NACK of the dispatcher.
    CHECK(peer.call(0x0777, req, rsp).errno_code() == ENOENT);
    // The peer takes no requests: ENOSYS from its link.
    CHECK(program.call(kEcho, req, rsp).errno_code() == ENOSYS);

    REQUIRE(program.push(kLevel, forge_test_level_report{-70}).ok());
    CHECK(forge::testing::wait_until([&] { return pushes == 1; }));

    const auto st = peer.stats();
    CHECK(st.framing.messages_received >= 8);
    CHECK(st.framing.frames_dropped == 0);
}

TEST_CASE("serial: no answer is a timeout; a line that goes away is EIO") {
    Pty pty;
    serial::LinkOptions o;
    o.fd = pty.fd;
    o.timeout = std::chrono::milliseconds(100);
    auto link = std::make_unique<serial::Link>(o);
    {
        // The other end is open but silent.
        const int other = ::open(pty.device.c_str(), O_RDWR | O_NOCTTY);
        REQUIRE(other >= 0);
        forge_test_echo_reply rsp{};
        CHECK(link->call(kEcho, forge_test_echo_request{}, rsp).errno_code() == ETIMEDOUT);
        ::close(other);
    }
    CHECK(forge::testing::wait_until([&] { return !link->connected(); }));
    forge_test_echo_reply rsp{};
    CHECK(link->call(kEcho, forge_test_echo_request{}, rsp).errno_code() == EIO);
}

TEST_CASE("serial: a device that is not there") {
    const auto why = [](const std::string& device, int baud) -> std::string {
        try {
            serial::Port p(device, baud);
        } catch (const std::exception& e) {
            return e.what();
        }
        return "";
    };
    CHECK(why("/dev/forge-no-such-tty", 115200).find("cannot open /dev/forge-no-such-tty") != std::string::npos);
    CHECK(why("/dev/null", 12345).find("not a standard baud") != std::string::npos);
    CHECK(why("/dev/null", 115200).find("not a terminal") != std::string::npos);
}
