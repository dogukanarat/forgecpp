// c2 and forge::Status. The c2 cases follow zephyrus's own unit tests
// (modules/connectivity/link/c2/test/unit) with the same bytes, so that both
// ends agree on every one of them.

#include <forge/c2.hpp>
#include <forge/status.hpp>
#include <forge/testing.hpp>

#include <array>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace c2 = forge::c2;
using forge::Status;

namespace {

Status c2_err(int code) { return Status::error(forge::module_id::c2, code); }

std::size_t build(std::uint8_t flags, std::uint16_t cmd_id, std::uint16_t seq, std::span<const std::uint8_t> payload,
                  std::span<std::uint8_t> out) {
    std::size_t n = 0;
    const auto hdr = c2::make_header(flags, cmd_id, seq, static_cast<std::uint16_t>(payload.size()));
    REQUIRE(c2::encode_message(hdr, payload, out, n).ok());
    return n;
}

}  // namespace

// --- header ----------------------------------------------------------------------------
TEST_CASE("c2: a built header carries the constants") {
    const auto hdr = c2::make_header(c2::flag::req, 0x1234, 5, 2);
    CHECK(hdr.version == c2::version);
    CHECK(hdr.flags == c2::flag::req);
    CHECK(hdr.cmd_id == 0x1234);
    CHECK(hdr.seq == 5);
    CHECK(hdr.length == 2);
}

TEST_CASE("c2: the wire bytes are the ones both ends agree on") {
    const std::array<std::uint8_t, 8> expected{
        0x01,        // version
        0x09,        // flags: req | ack
        0x12, 0x34,  // cmd_id
        0xAB, 0xCD,  // seq
        0x01, 0x00,  // length
    };
    std::array<std::uint8_t, 8> out{};
    REQUIRE(c2::encode_header(c2::make_header(c2::flag::req | c2::flag::ack, 0x1234, 0xABCD, 0x0100), out).ok());
    CHECK(out == expected);
}

TEST_CASE("c2: a header survives a roundtrip") {
    const auto sent = c2::make_header(c2::flag::rsp, 0xBEEF, 0x0102, 0x0304);
    std::array<std::uint8_t, c2::header_size> wire{};
    REQUIRE(c2::encode_header(sent, wire).ok());
    c2::Header received;
    REQUIRE(c2::decode_header(wire, received).ok());
    CHECK(received == sent);
}

TEST_CASE("c2: encode needs room for the whole header") {
    std::array<std::uint8_t, c2::header_size> out{};
    const auto hdr = c2::make_header(0, 0, 0, 0);
    CHECK(c2::encode_header(hdr, out).ok());
    CHECK(c2::encode_header(hdr, std::span(out).first(c2::header_size - 1)) == c2_err(EOVERFLOW));
}

TEST_CASE("c2: a short header is refused") {
    std::array<std::uint8_t, c2::header_size> wire{};
    c2::Header hdr;
    CHECK(c2::decode_header(std::span(wire).first(c2::header_size - 1), hdr) == c2_err(EMSGSIZE));
    CHECK(c2::decode_header({}, hdr) == c2_err(EMSGSIZE));
}

TEST_CASE("c2: an unknown version is refused") {
    std::array<std::uint8_t, c2::header_size> wire{};
    REQUIRE(c2::encode_header(c2::make_header(0, 0, 0, 0), wire).ok());
    // A field that moved between versions decodes to wrong data, not to an
    // error: the version is checked before any field is used.
    wire[0] = c2::version + 1;
    c2::Header hdr;
    hdr.cmd_id = 77;
    CHECK(c2::decode_header(wire, hdr) == c2_err(EPROTONOSUPPORT));
    CHECK(hdr.cmd_id == 77);  // untouched on failure
}

// --- message ---------------------------------------------------------------------------
TEST_CASE("c2: a message survives a roundtrip") {
    const std::array<std::uint8_t, 3> payload{0xAA, 0xBB, 0xCC};
    std::array<std::uint8_t, 32> wire{};
    const auto len = build(c2::flag::req, 0x0010, 7, payload, wire);
    CHECK(len == c2::header_size + payload.size());

    c2::Message m;
    REQUIRE(c2::decode_message(std::span(wire).first(len), m).ok());
    CHECK(m.header.cmd_id == 0x0010);
    CHECK(m.header.seq == 7);
    CHECK(m.header.length == payload.size());
    REQUIRE(m.payload.size() == payload.size());
    CHECK(std::memcmp(m.payload.data(), payload.data(), payload.size()) == 0);
    CHECK(m.payload.data() == wire.data() + c2::header_size);  // a view, not a copy
}

TEST_CASE("c2: a message with no payload has none to point at") {
    std::array<std::uint8_t, 32> wire{};
    const auto len = build(c2::flag::ack, 0x0011, 8, {}, wire);
    CHECK(len == c2::header_size);
    c2::Message m;
    REQUIRE(c2::decode_message(std::span(wire).first(len), m).ok());
    CHECK(m.header.length == 0);
    CHECK(m.payload.empty());
}

TEST_CASE("c2: a declared length that is not what arrived is refused") {
    const std::array<std::uint8_t, 3> payload{0xAA, 0xBB, 0xCC};
    std::array<std::uint8_t, 32> wire{};
    const auto len = build(c2::flag::req, 0x0010, 7, payload, wire);
    c2::Message m;
    // One byte missing: a cut payload must not pass as a whole one.
    CHECK(c2::decode_message(std::span(wire).first(len - 1), m) == c2_err(EMSGSIZE));
    // One byte too many: something else sticks to the message.
    CHECK(c2::decode_message(std::span(wire).first(len + 1), m) == c2_err(EMSGSIZE));
    CHECK(c2::decode_message(std::span(wire).first(len), m).ok());
}

TEST_CASE("c2: encode needs room for the payload too") {
    const std::array<std::uint8_t, 3> payload{0xAA, 0xBB, 0xCC};
    std::array<std::uint8_t, c2::header_size + 3> wire{};
    const auto hdr = c2::make_header(c2::flag::req, 0, 0, 3);
    std::size_t len = 0;
    REQUIRE(c2::encode_message(hdr, payload, wire, len).ok());
    CHECK(len == wire.size());
    CHECK(c2::encode_message(hdr, payload, std::span(wire).first(wire.size() - 1), len) == c2_err(EOVERFLOW));
}

TEST_CASE("c2: a declared payload needs bytes to take it from") {
    std::array<std::uint8_t, 32> wire{};
    std::size_t len = 0;
    CHECK(c2::encode_message(c2::make_header(c2::flag::req, 0, 0, 4), {}, wire, len) == c2_err(EINVAL));
    const std::array<std::uint8_t, 2> two{1, 2};
    CHECK(c2::encode_message(c2::make_header(c2::flag::req, 0, 0, 4), two, wire, len) == c2_err(EINVAL));
}

TEST_CASE("c2: a payload already in place is not disturbed") {
    // A caller that built its payload where it will be sent passes a view
    // into the same buffer: the copy overlaps itself exactly.
    std::array<std::uint8_t, 32> wire{};
    const std::array<std::uint8_t, 4> payload{0x01, 0x02, 0x03, 0x04};
    std::memcpy(&wire[c2::header_size], payload.data(), payload.size());
    std::size_t len = 0;
    REQUIRE(c2::encode_message(c2::make_header(c2::flag::rsp, 1, 2, 4), std::span(wire).subspan(c2::header_size, 4),
                               wire, len)
                .ok());
    CHECK(std::memcmp(&wire[c2::header_size], payload.data(), payload.size()) == 0);
}

TEST_CASE("c2: the request from the design example, byte for byte") {
    // radio set_frequency (id 0x0402): channel = rx1, frequency_hz = 2100000000
    const std::vector<std::uint8_t> payload{0x08, 0x01, 0x10, 0x80, 0xEA, 0xAD, 0xE9, 0x07};
    std::array<std::uint8_t, 32> wire{};
    const auto len = build(c2::flag::req, 0x0402, 42, payload, wire);
    const std::vector<std::uint8_t> expected{0x01, 0x01, 0x04, 0x02, 0x00, 0x2A, 0x00, 0x08,
                                             0x08, 0x01, 0x10, 0x80, 0xEA, 0xAD, 0xE9, 0x07};
    CHECK(std::vector<std::uint8_t>(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(len)) == expected);
}

TEST_CASE("c2: the kind of a message comes from its flags") {
    CHECK(c2::kind_of(c2::flag::req) == c2::Kind::request);
    CHECK(c2::kind_of(c2::flag::req | c2::flag::ack) == c2::Kind::request);  // a request wins
    CHECK(c2::kind_of(c2::flag::rsp) == c2::Kind::reply);
    CHECK(c2::kind_of(c2::flag::rsp | c2::flag::more) == c2::Kind::reply);
    CHECK(c2::kind_of(c2::flag::ack) == c2::Kind::ack);
    CHECK(c2::kind_of(c2::flag::nack) == c2::Kind::nack);
    CHECK(c2::kind_of(c2::flag::push) == c2::Kind::push);
    CHECK(c2::kind_of(0) == c2::Kind::unknown);
    CHECK(c2::kind_of(c2::flag::proto) == c2::Kind::unknown);
}

// --- rendering -------------------------------------------------------------------------
TEST_CASE("c2: flags render as a list") {
    CHECK(c2::flags_to_string(c2::flag::req) == "req");
    CHECK(c2::flags_to_string(c2::flag::req | c2::flag::ack) == "req|ack");
    CHECK(c2::flags_to_string(0) == "none");
    // A flag this version has no name for is shown, not dropped.
    CHECK(c2::flags_to_string(0x80) == "0x80");
    CHECK(c2::flags_to_string(c2::flag::push | 0x80) == "push|0x80");
    CHECK(c2::flags_to_string(0xFF) == "req|rsp|push|ack|nack|proto|more|0x80");
    CHECK(c2::flags_to_string(0xFF).size() < 40);  // zephyrus's C2_FLAGS_STR_LEN
}

// --- NACK and status -------------------------------------------------------------------
TEST_CASE("status: the bit layout of zephyrus") {
    CHECK(Status().ok());
    CHECK(Status().wire() == 0);
    CHECK(Status().to_string() == "ok");

    // module 0x0100 (forge c2), EINVAL (22): 1 | 0x0100 | 0x0016
    const auto s = Status::error(forge::module_id::c2, EINVAL);
    CHECK_FALSE(s.ok());
    CHECK(static_cast<std::uint32_t>(s.wire()) == 0x81000016U);
    CHECK(s.wire() < 0);  // always negative
    CHECK(s.module() == forge::module_id::c2);
    CHECK(s.wire_code() == 22);
    CHECK(s.errno_code() == EINVAL);
    CHECK(Status::error(forge::module_id::c2, -EINVAL) == s);  // the sign does not matter

    // Module 0, code 0 is still an error (bit 31).
    CHECK_FALSE(Status::wire_error(0, 0).ok());
    // Positive values (counts) are success, as in zephyrus.
    CHECK(Status::from_wire(5).ok());
    CHECK(Status::from_wire(5).wire() == 0);
}

TEST_CASE("status: a plain negative errno has no origin") {
    // Zephyr and libc return -EINVAL; sign extension sets every module bit.
    const auto s = Status::from_wire(-22);
    CHECK_FALSE(s.ok());
    CHECK(s.module() == forge::module_id::unknown);
    CHECK(s.wire_code() == 22);
    CHECK(s.errno_code() == EINVAL);
}

TEST_CASE("status: errno numbers on the wire are Zephyr's") {
    // Different on Linux and Zephyr (newlib / picolibc numbering).
    CHECK(forge::errno_to_wire(ENOSYS) == 88);
    CHECK(forge::errno_to_wire(EMSGSIZE) == 122);
    CHECK(forge::errno_to_wire(ETIMEDOUT) == 116);
    CHECK(forge::errno_to_wire(EOVERFLOW) == 139);
    CHECK(forge::errno_to_wire(EPROTONOSUPPORT) == 123);
    CHECK(forge::errno_to_wire(ENOTSUP) == 134);
    CHECK(forge::errno_to_wire(ECANCELED) == 140);
    // The same on both.
    CHECK(forge::errno_to_wire(EINVAL) == 22);
    CHECK(forge::errno_to_wire(ENOENT) == 2);
    CHECK(forge::errno_to_wire(ENOMEM) == 12);
    // Back to the host's numbers.
    for (int e : {ENOSYS, EMSGSIZE, ETIMEDOUT, EOVERFLOW, EINVAL, ENOENT, ECONNRESET, EBADMSG, EAGAIN}) {
        CHECK(forge::errno_from_wire(forge::errno_to_wire(e)) == e);
    }
    // Zephyr's EOPNOTSUPP (95) and ENOTSUP (134) are one value on Linux.
    CHECK(forge::errno_from_wire(95) == EOPNOTSUPP);
    CHECK(forge::errno_from_wire(134) == ENOTSUP);
    // Unknown codes become EIO rather than a wrong errno.
    CHECK(forge::errno_from_wire(9999) == EIO);
    CHECK(forge::errno_to_wire(4095) == 5);

    // A status from a zephyrus device: module 0x0012, ENOSYS in Zephyr's numbers.
    const auto remote = Status::wire_error(0x0012, 88);
    CHECK(remote.errno_code() == ENOSYS);
    CHECK(remote.http_status() == 501);
}

TEST_CASE("status: a NACK carries the status, big-endian") {
    const auto s = Status::wire_error(0x0012, 88);  // 0x80120058
    const auto p = c2::nack_payload(s);
    CHECK(p == std::array<std::uint8_t, 4>{0x80, 0x12, 0x00, 0x58});

    Status back;
    REQUIRE(c2::nack_status(p, back).ok());
    CHECK(back == s);
    CHECK(back.module() == 0x0012);

    const std::array<std::uint8_t, 3> short_payload{0x80, 0x12, 0x00};
    CHECK(c2::nack_status(short_payload, back) == c2_err(EMSGSIZE));
}

TEST_CASE("status: text and HTTP status") {
    const auto s = Status::error(forge::module_id::c2, EMSGSIZE, "declared payload length is not what arrived");
    CHECK(s.to_string() == "c2: declared payload length is not what arrived (EMSGSIZE)");
    CHECK(s.http_status() == 400);
    CHECK(Status::error(forge::module_id::c2, ENOENT).to_string().find("c2: ") == 0);
    CHECK(Status::error(forge::module_id::c2, ENOENT).to_string().find("(ENOENT)") != std::string::npos);
    CHECK(Status::wire_error(0x0042, 22).to_string().find("module 0x0042: ") == 0);
    forge::name_module(0x0042, "power");
    CHECK(Status::wire_error(0x0042, 22).to_string().find("power: ") == 0);
    CHECK(Status::from_wire(-2).to_string().find("(ENOENT)") != std::string::npos);  // no origin, no prefix
    CHECK(Status::wire_error(forge::module_id::nrf, 515).to_string() == "nrf: modem error (code 515)");

    CHECK(Status().http_status() == 200);
    CHECK(Status::error(1, EINVAL).http_status() == 400);
    CHECK(Status::error(1, EACCES).http_status() == 403);
    CHECK(Status::error(1, ENAMETOOLONG).http_status() == 400);  // a text too long for its field
    CHECK(Status::error(1, ENOENT).http_status() == 404);
    CHECK(Status::error(1, EBUSY).http_status() == 409);
    CHECK(Status::error(1, ENOSYS).http_status() == 501);
    CHECK(Status::error(1, ENOMEM).http_status() == 503);
    CHECK(Status::error(1, ETIMEDOUT).http_status() == 504);
    CHECK(Status::error(1, EIO).http_status() == 500);
}

TEST_CASE("status: a message that is not a literal is copied and kept") {
    using forge::Status;
    const char* kept = nullptr;
    {
        const std::string text = "frequency " + std::to_string(2400) + " MHz is too high";
        const auto s = Status::error(forge::module_id::app, ERANGE, text);  // a std::string: copied
        kept = s.message();
        CHECK(kept != text.c_str());
    }
    CHECK(std::string(kept) == "frequency 2400 MHz is too high");  // the std::string is gone, the message is not
    CHECK(Status::error(forge::module_id::app, ERANGE, "too high").message() == std::string("too high"));
    // The same text is stored once.
    CHECK(Status::error(1, EINVAL, std::string("same")).message() ==
          Status::error(2, EIO, std::string("same")).message());
    CHECK(forge::intern_message("x") == forge::intern_message(std::string("x")));
}
