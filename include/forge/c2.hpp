#pragma once
// c2: the command-and-control message on the wire, byte for byte the one of
// zephyrus (modules/connectivity/link/c2), so that forgecpp and zephyrus
// devices talk to each other directly.
//
// Eight header bytes, then the payload (a nanopb / protobuf message):
//
//   version(1) | flags(1) | cmd_id(2) | seq(2) | length(2) | payload ...
//
// Multi-byte fields are big-endian. The transport gives exactly one whole
// message whose integrity it has checked: no sync word, no checksum and no
// address here (a UART adds framing and a CRC, CAN uses ISO-TP, ...).
//
//   auto hdr = forge::c2::make_header(forge::c2::flag::req, 0x0402, 42, payload.size());
//   std::array<std::uint8_t, 64> out;
//   std::size_t n = 0;
//   forge::c2::encode_message(hdr, payload, out, n);
//
//   forge::c2::Message m;
//   if (auto s = forge::c2::decode_message(received, m); !s.ok()) drop(s);
//   dispatch(m.header.cmd_id, m.payload);
//
// Nothing here allocates; every function returns a forge::Status (module c2).

#include "forge/status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace forge::c2 {

/// The version that a sender writes and that a receiver requires.
inline constexpr std::uint8_t version = 1;
/// Header bytes in front of every payload.
inline constexpr std::size_t header_size = 8;
/// Largest payload a header can declare (the length field is 16 bits).
inline constexpr std::size_t payload_max = 0xFFFF;
/// Payload bytes of a NACK: the status, big-endian.
inline constexpr std::size_t nack_payload_size = 4;

/// The flag byte: exactly one of the first five gives the kind of a message;
/// `proto` and `more` add to it.
namespace flag {
inline constexpr std::uint8_t req = 1U << 0;    ///< a request; it expects an answer
inline constexpr std::uint8_t rsp = 1U << 1;    ///< the answer to a request, with a payload
inline constexpr std::uint8_t push = 1U << 2;   ///< sent without a request; nothing answers it
inline constexpr std::uint8_t ack = 1U << 3;    ///< the request succeeded, no data to send back
inline constexpr std::uint8_t nack = 1U << 4;   ///< the request failed; payload = 4-byte status
inline constexpr std::uint8_t proto = 1U << 5;  ///< the payload is a protocol message, not command data
inline constexpr std::uint8_t more = 1U << 6;   ///< with rsp: more answers with this seq follow
}  // namespace flag

/// One header, in host form (not the wire layout: that is written field by field).
struct Header {
    std::uint8_t version = c2::version;
    std::uint8_t flags = 0;
    std::uint16_t cmd_id = 0;
    std::uint16_t seq = 0;
    std::uint16_t length = 0;  ///< payload bytes after the header

    friend bool operator==(const Header&, const Header&) = default;
};

/// A header with the current version and the given fields.
constexpr Header make_header(std::uint8_t flags, std::uint16_t cmd_id, std::uint16_t seq, std::uint16_t length) {
    return Header{c2::version, flags, cmd_id, seq, length};
}

/// The eight wire bytes of `hdr` (its version is written as it is).
/// EOVERFLOW: `out` is shorter than header_size.
Status encode_header(const Header& hdr, std::span<std::uint8_t> out);

/// Read eight wire bytes. EMSGSIZE: fewer bytes; EPROTONOSUPPORT: another version.
Status decode_header(std::span<const std::uint8_t> in, Header& out);

/// Header then payload. `payload` must hold exactly `hdr.length` bytes (EINVAL)
/// and may already sit at out[header_size] (it is moved, not copied).
/// EOVERFLOW: `out` is too short. `written`: header_size + hdr.length.
Status encode_message(const Header& hdr, std::span<const std::uint8_t> payload, std::span<std::uint8_t> out,
                      std::size_t& written);

/// A received message: its header and a view of its payload inside the buffer.
struct Message {
    Header header;
    std::span<const std::uint8_t> payload;  ///< empty when the message has none
};

/// Exactly one message, nothing before or after. EMSGSIZE when the declared
/// length is not the number of bytes after the header (a short or a long
/// message is refused, never cut); otherwise as decode_header().
Status decode_message(std::span<const std::uint8_t> in, Message& out);

/// What a receiver does with a message, from its flags.
enum class Kind { request, reply, ack, nack, push, unknown };
/// A request wins (as in zephyrus's dispatcher); then nack, ack, reply, push.
Kind kind_of(std::uint8_t flags);

/// "req|ack", "none", "push|0x80": unknown bits stay visible, in hex.
std::string flags_to_string(std::uint8_t flags);

/// The payload of a NACK for `status` (big-endian int32).
std::array<std::uint8_t, nack_payload_size> nack_payload(const Status& status);
/// The status a NACK carries. EMSGSIZE (module c2) when the payload is not four bytes.
Status nack_status(std::span<const std::uint8_t> payload, Status& carried);

}  // namespace forge::c2
