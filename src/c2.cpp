#include "forge/c2.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace forge::c2 {
namespace {

Status fail(int errno_code, const char* message) { return Status::error(module_id::kC2, errno_code, message); }

void put_u16(std::uint8_t* out, std::uint16_t v) {
    out[0] = static_cast<std::uint8_t>(v >> 8);
    out[1] = static_cast<std::uint8_t>(v & 0xFFU);
}

std::uint16_t get_u16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) | in[1]);
}

struct FlagName {
    std::uint8_t bit;
    const char* name;
};
constexpr FlagName kFlagNames[] = {{flag::kReq, "req"},   {flag::kRsp, "rsp"},     {flag::kPush, "push"},
                                   {flag::kAck, "ack"},   {flag::kNack, "nack"},   {flag::kProto, "proto"},
                                   {flag::kMore, "more"}};

}  // namespace

Status encode_header(const Header& hdr, std::span<std::uint8_t> out) {
    if (out.size() < kHeaderSize) return fail(EOVERFLOW, "no room for the c2 header");
    out[0] = hdr.version;
    out[1] = hdr.flags;
    put_u16(&out[2], hdr.cmd_id);
    put_u16(&out[4], hdr.seq);
    put_u16(&out[6], hdr.length);
    return {};
}

Status decode_header(std::span<const std::uint8_t> in, Header& out) {
    if (in.size() < kHeaderSize) return fail(EMSGSIZE, "shorter than a c2 header");
    Header hdr;
    hdr.version = in[0];
    hdr.flags = in[1];
    hdr.cmd_id = get_u16(&in[2]);
    hdr.seq = get_u16(&in[4]);
    hdr.length = get_u16(&in[6]);
    if (hdr.version != c2::kVersion) return fail(EPROTONOSUPPORT, "another c2 version");
    out = hdr;
    return {};
}

Status encode_message(const Header& hdr, std::span<const std::uint8_t> payload, std::span<std::uint8_t> out,
                      std::size_t& written) {
    if (payload.size() != hdr.length) return fail(EINVAL, "payload size is not the header's length");
    if (out.size() < kHeaderSize + hdr.length) return fail(EOVERFLOW, "no room for the c2 message");
    if (auto s = encode_header(hdr, out); !s.ok()) return s;
    // memmove, not memcpy: the payload may already be in place at out[8].
    if (hdr.length != 0) std::memmove(&out[kHeaderSize], payload.data(), hdr.length);
    written = kHeaderSize + hdr.length;
    return {};
}

Status decode_message(std::span<const std::uint8_t> in, Message& out) {
    Header hdr;
    if (auto s = decode_header(in, hdr); !s.ok()) return s;
    if (in.size() != kHeaderSize + hdr.length) {
        return fail(EMSGSIZE, "declared payload length is not what arrived");
    }
    out.header = hdr;
    out.payload = hdr.length != 0 ? in.subspan(kHeaderSize, hdr.length) : std::span<const std::uint8_t>{};
    return {};
}

Kind kind_of(std::uint8_t flags) {
    if (flags & flag::kReq) return Kind::Request;
    if (flags & flag::kNack) return Kind::Nack;
    if (flags & flag::kAck) return Kind::Ack;
    if (flags & flag::kRsp) return Kind::Reply;
    if (flags & flag::kPush) return Kind::Push;
    return Kind::Unknown;
}

std::string flags_to_string(std::uint8_t flags) {
    if (flags == 0) return "none";
    std::string out;
    std::uint8_t remaining = flags;
    for (const auto& f : kFlagNames) {
        if (!(remaining & f.bit)) continue;
        if (!out.empty()) out += '|';
        out += f.name;
        remaining = static_cast<std::uint8_t>(remaining & ~f.bit);
    }
    if (remaining != 0) {
        char hex[8];
        std::snprintf(hex, sizeof(hex), "0x%02x", remaining);
        if (!out.empty()) out += '|';
        out += hex;
    }
    return out;
}

std::array<std::uint8_t, kNackPayloadSize> nack_payload(const Status& status) {
    const auto raw = static_cast<std::uint32_t>(status.wire());
    return {static_cast<std::uint8_t>(raw >> 24), static_cast<std::uint8_t>(raw >> 16),
            static_cast<std::uint8_t>(raw >> 8), static_cast<std::uint8_t>(raw & 0xFFU)};
}

Status nack_status(std::span<const std::uint8_t> payload, Status& carried) {
    if (payload.size() != kNackPayloadSize) return fail(EMSGSIZE, "a NACK carries four bytes");
    const std::uint32_t raw = (static_cast<std::uint32_t>(payload[0]) << 24) |
                              (static_cast<std::uint32_t>(payload[1]) << 16) |
                              (static_cast<std::uint32_t>(payload[2]) << 8) | payload[3];
    carried = Status::from_wire(static_cast<std::int32_t>(raw));
    return {};
}

}  // namespace forge::c2
