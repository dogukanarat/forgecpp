// forge::serial against zephyrus's modules/lib/cobs: the same message gives
// the same bytes on the wire, and the same damaged byte stream gives the same
// messages on both ends. Frames are 64 bytes here (CMakeLists.txt), so a
// message takes many fragments.

extern "C" {
#include <zephyrus/cobs/cobs.h>
#include <zephyrus/cobs/cobs_framing.h>
#include <zephyrus/common_error.h>
}

#include <forge/serial.hpp>
#include <forge/testing.hpp>

#include <random>
#include <vector>

namespace serial = forge::serial;
using Bytes = std::vector<std::uint8_t>;

namespace {

constexpr int kRounds = 3000;

COBS_FRAMING_DEFINE(zlink);

Bytes z_wire;                   // what zephyrus sent
std::vector<Bytes> z_messages;  // what zephyrus received

int32_t z_send(cobs_framing_t*, const uint8_t* frame, size_t len, void*) {
    z_wire.insert(z_wire.end(), frame, frame + len);
    return STATUS_OK;
}

int32_t z_deliver(cobs_framing_t*, uint8_t* data, size_t len, void*) {
    z_messages.emplace_back(data, data + len);
    return STATUS_OK;
}

const cobs_framing_callbacks_t z_callbacks = {z_send, z_deliver};

cobs_framing_t* zephyrus() {
    static const bool ready = [] {
        REQUIRE(cobs_framing_init(&zlink, &z_callbacks, nullptr) == STATUS_OK);
        return true;
    }();
    (void)ready;
    return &zlink;
}

serial::FramingOptions same_as_zephyrus() {
    return {.frame_size = COBS_FRAME_SIZE, .max_message = COBS_MAX_MESSAGE_SIZE};
}

Bytes random_bytes(std::mt19937& rng, std::size_t n) {
    Bytes b(n);
    const bool zeros = rng() % 2 == 0;  // half of them full of zeros, the hard case for COBS
    for (auto& x : b) x = static_cast<std::uint8_t>(zeros ? rng() % 4 : rng());
    return b;
}

}  // namespace

TEST_CASE("zephyrus compat: CRC16 and one frame") {
    std::mt19937 rng(11);
    for (int i = 0; i < kRounds; ++i) {
        const auto payload = random_bytes(rng, rng() % 700);
        CHECK(serial::crc16(payload) == cobs_crc16_ccitt(payload.data(), payload.size()));

        Bytes zout(payload.size() + payload.size() / 254 + 16);
        size_t zlen = 0;
        REQUIRE(cobs_encode_frame(payload.data(), payload.size(), zout.data(), zout.size(), &zlen) == STATUS_OK);
        zout.resize(zlen);
        Bytes fout;
        serial::encode_frame(payload, fout);
        REQUIRE(fout == zout);
    }
}

TEST_CASE("zephyrus compat: a message in fragments") {
    std::mt19937 rng(12);
    auto* z = zephyrus();
    serial::Framing f(same_as_zephyrus());
    CHECK(f.fragment_payload() == COBS_FRAMING_MAX_PAYLOAD);
    for (int i = 0; i < kRounds; ++i) {
        const auto message = random_bytes(rng, 1 + rng() % 1500);
        z_wire.clear();
        REQUIRE(cobs_framing_send(z, message.data(), message.size()) == STATUS_OK);
        Bytes fwire;
        REQUIRE(f.encode(message, fwire).ok());
        REQUIRE(fwire == z_wire);
    }
    // The same refusals.
    Bytes big(COBS_MAX_MESSAGE_SIZE + 1), fwire;
    CHECK(err_get_code(cobs_framing_send(z, big.data(), big.size())) == EMSGSIZE);
    CHECK(f.encode(big, fwire).errno_code() == EMSGSIZE);
    CHECK(f.encode({}, fwire).errno_code() == EINVAL);  // zephyrus: EINVAL for len 0 too
}

TEST_CASE("zephyrus compat: the same messages from a damaged stream") {
    std::mt19937 rng(13);
    auto* z = zephyrus();
    serial::Framing f(same_as_zephyrus());
    int delivered = 0;
    for (int round = 0; round < kRounds / 3; ++round) {
        // A few messages from zephyrus, then damage on the wire.
        z_wire.clear();
        const int count = 1 + static_cast<int>(rng() % 5);
        for (int i = 0; i < count; ++i) {
            const auto m = random_bytes(rng, 1 + rng() % 400);
            REQUIRE(cobs_framing_send(z, m.data(), m.size()) == STATUS_OK);
        }
        Bytes wire = z_wire;
        const int damage = static_cast<int>(rng() % 4);
        for (int d = 0; d < damage && !wire.empty(); ++d) {
            const auto at = rng() % wire.size();
            switch (rng() % 4) {
                case 0: wire[at] ^= static_cast<std::uint8_t>(1 + rng() % 255); break;      // a flipped byte
                case 1: wire.erase(wire.begin() + static_cast<std::ptrdiff_t>(at)); break;  // a lost byte
                case 2: wire.insert(wire.begin() + static_cast<std::ptrdiff_t>(at), 0); break;  // a false boundary
                default: {                                                                     // a burst of noise
                    const auto noise = random_bytes(rng, rng() % 80);
                    wire.insert(wire.begin() + static_cast<std::ptrdiff_t>(at), noise.begin(), noise.end());
                }
            }
        }
        // Both ends read it in the same random pieces.
        z_messages.clear();
        std::vector<Bytes> fmessages;
        for (std::size_t at = 0; at < wire.size();) {
            const auto n = std::min<std::size_t>(1 + rng() % 100, wire.size() - at);
            REQUIRE(cobs_framing_received(z, wire.data() + at, n) == STATUS_OK);
            f.receive(std::span(wire).subspan(at, n),
                      [&](std::span<const std::uint8_t> m) { fmessages.emplace_back(m.begin(), m.end()); });
            at += n;
        }
        REQUIRE(fmessages == z_messages);
        delivered += static_cast<int>(fmessages.size());
    }
    CHECK(delivered > kRounds / 3);  // most of them still arrive
}
