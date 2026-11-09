// Bytes from a serial line: the COBS framing (forge/serial.hpp) must take any
// byte stream without delivering what was never sent whole.
//
// Properties: the receiver never delivers a message longer than max_message;
// a message that is encoded and fed back arrives once and unchanged, with
// the rest of the input around it; the counters add up; and a frame that
// decodes encodes back to the same bytes.
#include "forge/serial.hpp"
#include "fuzz.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

using namespace forge;
using Bytes = std::vector<std::uint8_t>;

namespace {
constexpr std::size_t kFrame = 64, kMaxMessage = 1024;
}  // namespace

const std::vector<std::string>& seeds() {
    static const std::vector<std::string> s = [] {
        std::vector<std::string> out;
        serial::Framing tx({.frame_size = kFrame, .max_message = kMaxMessage});
        for (const char* text : {"a", "hello, zephyrus", "a message in several fragments: 0123456789 0123456789"}) {
            Bytes wire;
            const std::string_view sv(text);
            (void)tx.encode(Bytes(sv.begin(), sv.end()), wire);
            out.emplace_back(wire.begin(), wire.end());
        }
        out.emplace_back("\x00\x01\x00noise\x00");
        return out;
    }();
    return s;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> in(data, size);
    serial::Framing rx({.frame_size = kFrame, .max_message = kMaxMessage});

    // 1. Any bytes: nothing longer than the limit, nothing crashes.
    std::size_t delivered = 0;
    rx.receive(in, [&](std::span<const std::uint8_t> m) {
        ++delivered;
        if (m.empty() || m.size() > kMaxMessage) fuzz_fail("delivered a message outside the bounds");
    });
    const auto st = rx.stats();
    if (st.messages_received != delivered) fuzz_fail("messages_received does not count what was delivered");

    // 2. The input as a message (when it fits) survives a round trip: exactly
    //    once on a clean line; at least once when it is sent twice with the raw
    //    input before and after (noise that holds a 0x00 can take the first
    //    frame after it with it: the delimiter that ends the noise does not
    //    start that frame; zephyrus decides the same way).
    if (!in.empty() && in.size() <= kMaxMessage) {
        serial::Framing tx({.frame_size = kFrame, .max_message = kMaxMessage});
        Bytes wire;
        if (!tx.encode(in, wire).ok()) fuzz_fail("a message within the bounds was not encoded");
        const Bytes message(in.begin(), in.end());
        const auto count = [&](const Bytes& line, std::size_t piece) {
            serial::Framing rx2({.frame_size = kFrame, .max_message = kMaxMessage});
            std::size_t n = 0;
            // Fed in pieces, as a UART would.
            for (std::size_t at = 0; at < line.size(); at += piece) {
                const auto len = std::min(piece, line.size() - at);
                rx2.receive(std::span(line).subspan(at, len), [&](std::span<const std::uint8_t> m) {
                    if (Bytes(m.begin(), m.end()) == message) ++n;
                });
            }
            return n;
        };
        const std::size_t piece = 1 + (in.size() % 97);
        if (count(wire, piece) != 1) fuzz_fail("a message on a clean line did not arrive exactly once");
        Bytes noisy(in.begin(), in.end());
        noisy.insert(noisy.end(), wire.begin(), wire.end());
        noisy.insert(noisy.end(), wire.begin(), wire.end());
        noisy.insert(noisy.end(), in.begin(), in.end());
        if (count(noisy, piece) < 1) fuzz_fail("a message sent twice after noise did not arrive at all");
    }

    // 3. One frame: what decodes, encodes back to the same bytes.
    if (size >= 2 && data[0] == 0 && data[size - 1] == 0 &&
        std::find(data + 1, data + size - 1, 0) == data + size - 1) {
        Bytes payload;
        if (serial::decode_frame(in.subspan(1, size - 2), payload).ok()) {
            Bytes again;
            serial::encode_frame(payload, again);
            if (again.size() != size || std::memcmp(again.data(), data, size) != 0) {
                fuzz_fail("a decoded frame does not encode back to the same bytes");
            }
        }
    }
    return 0;
}
