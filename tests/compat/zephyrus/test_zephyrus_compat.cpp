// forgecpp against zephyrus's own C code: the same input must give the same
// bytes and the same decisions on both ends of a link.
//
// Built only with -DFORGE_ZEPHYRUS_DIR=<a zephyrus checkout>. The zephyrus
// sources (c2.c, common_error.c) compile on the host with the stubs in
// stubs/; they keep the host's errno numbers, so the codes are compared as
// host errno values.

extern "C" {
#include <zephyrus/c2/c2.h>
#include <zephyrus/common_error.h>
}

#include <forge/c2.hpp>
#include <forge/status.hpp>
#include <forge/testing.hpp>

#include <cstring>
#include <random>
#include <vector>

namespace c2 = forge::c2;

namespace {

constexpr int kRounds = 100000;

int32_t zephyrus_errno(int32_t status) { return status == STATUS_OK ? 0 : err_get_code(status); }
int32_t forge_errno(const forge::Status& s) { return s.ok() ? 0 : s.errno_code(); }

}  // namespace

TEST_CASE("zephyrus compat: header bytes") {
    std::mt19937 rng(1);
    for (int i = 0; i < kRounds; ++i) {
        const auto version = static_cast<uint8_t>(rng() % 3);
        const auto flags = static_cast<uint8_t>(rng());
        const auto cmd_id = static_cast<uint16_t>(rng());
        const auto seq = static_cast<uint16_t>(rng());
        const auto length = static_cast<uint16_t>(rng());
        const auto out_size = static_cast<size_t>(rng() % 12);

        c2_header_t zh = {version, flags, cmd_id, seq, length};
        uint8_t zout[16] = {0};
        const int32_t zs = c2_header_encode(&zh, zout, out_size);

        const c2::Header fh{version, flags, cmd_id, seq, length};
        uint8_t fout[16] = {0};
        const auto fs = c2::encode_header(fh, std::span<uint8_t>(fout, out_size));

        REQUIRE(zephyrus_errno(zs) == forge_errno(fs));
        REQUIRE(std::memcmp(zout, fout, sizeof(zout)) == 0);
    }
}

TEST_CASE("zephyrus compat: message bytes") {
    std::mt19937 rng(2);
    for (int i = 0; i < kRounds; ++i) {
        const auto length = static_cast<uint16_t>(rng() % 24);
        std::vector<uint8_t> payload(length);
        for (auto& b : payload) b = static_cast<uint8_t>(rng());
        const auto out_size = static_cast<size_t>(rng() % 40);
        const auto flags = static_cast<uint8_t>(rng());
        const auto cmd_id = static_cast<uint16_t>(rng());
        const auto seq = static_cast<uint16_t>(rng());

        c2_header_t zh = {0};
        REQUIRE(c2_header_init(&zh, flags, cmd_id, seq, length) == STATUS_OK);
        uint8_t zout[48] = {0};
        size_t zlen = 0;
        const int32_t zs = c2_message_encode(&zh, payload.data(), zout, out_size, &zlen);

        uint8_t fout[48] = {0};
        size_t flen = 0;
        const auto fs = c2::encode_message(c2::make_header(flags, cmd_id, seq, length), payload,
                                           std::span<uint8_t>(fout, out_size), flen);

        REQUIRE(zephyrus_errno(zs) == forge_errno(fs));
        REQUIRE(std::memcmp(zout, fout, sizeof(zout)) == 0);
        if (zs == STATUS_OK) REQUIRE(zlen == flen);
    }
}

TEST_CASE("zephyrus compat: decoding the same received bytes") {
    std::mt19937 rng(3);
    for (int i = 0; i < kRounds; ++i) {
        // A real buffer also for zero bytes: an empty vector has no pointer, and
        // zephyrus refuses a NULL pointer (EINVAL) before it looks at the size.
        const auto size = static_cast<size_t>(rng() % 32);
        uint8_t storage[32] = {0};
        for (size_t k = 0; k < size; ++k) storage[k] = static_cast<uint8_t>(rng());
        const std::span<uint8_t> in(storage, size);
        // Make most of them plausible: the right version and a length near the size.
        if (size >= 8 && rng() % 4 != 0) {
            in[0] = 1;
            const auto declared = static_cast<uint16_t>(size - 8 + (rng() % 3) - 1);
            in[6] = static_cast<uint8_t>(declared >> 8);
            in[7] = static_cast<uint8_t>(declared);
        }

        c2_header_t zh = {0};
        const uint8_t* zpayload = reinterpret_cast<const uint8_t*>(1);
        const int32_t zs = c2_message_decode(storage, size, &zh, &zpayload);

        c2::Message fm;
        const auto fs = c2::decode_message(in, fm);

        REQUIRE(zephyrus_errno(zs) == forge_errno(fs));
        if (zs != STATUS_OK) continue;
        REQUIRE(zh.version == fm.header.version);
        REQUIRE(zh.flags == fm.header.flags);
        REQUIRE(zh.cmd_id == fm.header.cmd_id);
        REQUIRE(zh.seq == fm.header.seq);
        REQUIRE(zh.length == fm.header.length);
        if (zh.length == 0) {
            REQUIRE(zpayload == nullptr);
            REQUIRE(fm.payload.empty());
        } else {
            REQUIRE(zpayload == fm.payload.data());
            REQUIRE(zh.length == fm.payload.size());
        }
    }
}

TEST_CASE("zephyrus compat: flag text") {
    for (unsigned flags = 0; flags < 256; ++flags) {
        char buf[C2_FLAGS_STR_LEN] = {0};
        REQUIRE(c2_flags_to_str(static_cast<uint8_t>(flags), buf, sizeof(buf)) == STATUS_OK);
        REQUIRE(c2::flags_to_string(static_cast<uint8_t>(flags)) == buf);
    }
}

TEST_CASE("zephyrus compat: the constants") {
    CHECK(c2::version == C2_VERSION);
    CHECK(c2::header_size == C2_HEADER_SIZE);
    CHECK(c2::payload_max == C2_PAYLOAD_MAX);
    CHECK(c2::nack_payload_size == C2_NACK_PAYLOAD_SIZE);
    CHECK(c2::flag::req == C2_FLAG_REQ);
    CHECK(c2::flag::rsp == C2_FLAG_RSP);
    CHECK(c2::flag::push == C2_FLAG_PUSH);
    CHECK(c2::flag::ack == C2_FLAG_ACK);
    CHECK(c2::flag::nack == C2_FLAG_NACK);
    CHECK(c2::flag::proto == C2_FLAG_PROTO);
    CHECK(c2::flag::more == C2_FLAG_MORE);
    CHECK(forge::module_id::unknown == MODULE_ID_UNKNOWN);
    CHECK(forge::module_id::app == MODULE_ID_APP);
    CHECK(forge::module_id::external_base == MODULE_ID_EXTERNAL_BASE);
    CHECK(forge::module_id::nrf == MODULE_ID_NRF);
    CHECK(forge::module_id::zephyr == MODULE_ID_ZEPHYR);
    CHECK(forge::module_id::raw == MODULE_ID_RAW);
    CHECK(forge::module_id::max == MODULE_ID_MAX);
    // forgecpp's own module ids are outside the range of zephyrus's modules.
    CHECK(forge::module_id::forge_base >= MODULE_ID_EXTERNAL_BASE);
}

TEST_CASE("zephyrus compat: status encoding") {
    std::mt19937 rng(4);
    for (int i = 0; i < kRounds; ++i) {
        const auto module = static_cast<uint16_t>(rng() % 0x8000);
        const auto code = static_cast<uint16_t>(rng());
        REQUIRE(err_encode(module, code) == forge::Status::wire_error(module, code).wire());
        REQUIRE(err_encode(module, -static_cast<int32_t>(code)) == forge::Status::wire_error(module, code).wire());
    }
}

TEST_CASE("zephyrus compat: status decoding, every kind of value") {
    std::mt19937 rng(5);
    std::vector<int32_t> values{0,
                                1,
                                -1,
                                -22,
                                -0x8000,
                                -0xFFFF,
                                -0x10000,
                                INT32_MIN,
                                INT32_MAX,
                                static_cast<int32_t>(0x80000000U),
                                static_cast<int32_t>(0xFFFF0000U)};
    for (int i = 0; i < kRounds; ++i) values.push_back(static_cast<int32_t>(rng()));
    for (int i = 0; i < 1000; ++i) values.push_back(-static_cast<int32_t>(rng() % 70000));  // plain errnos
    for (int32_t v : values) {
        const auto s = forge::Status::from_wire(v);
        REQUIRE((v < 0) == !s.ok());
        REQUIRE(err_get_module_id(v) == s.module());
        REQUIRE(err_get_code(v) == s.wire_code());
    }
}
