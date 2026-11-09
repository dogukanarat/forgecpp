// A c2 frame from a link: decoding it, and what the dispatcher answers.
//
// Properties: a frame that decodes encodes back to the same bytes; every
// answer is itself a valid frame for the same command and sequence number,
// and nothing is answered to what is not a request.
#include "forge/c2.hpp"
#include "forge/tmtc.hpp"
#include "forge_test/commands.forge.hpp"
#include "fuzz.hpp"

#include <cstring>
#include <vector>

using namespace forge;

namespace {

constexpr std::uint16_t kEcho = 0x0101, kCount = 0x0102, kFail = 0x0103, kLevel = 0x0180;

const tmtc::Registry& commands() {
    static const auto registry = [] {
        auto r = std::make_unique<tmtc::Registry>();
        r->add<forge_test_echo_request, forge_test_echo_reply>(kEcho, "echo", [](const auto& req, auto& rsp) {
            rsp.token = req.token;
            return pb::assign(rsp.text, pb::view(req.text));
        });
        r->add_parts<forge_test_count_request, forge_test_count_reply>(
            kCount, "count", [](const auto& req, auto& rsp, std::uint32_t& more) {
            rsp.value = more + 1;
            more = rsp.value < req.to && rsp.value < 50 ? rsp.value : 0;  // at most 50 parts
            return Status();
        });
        r->add<forge_test_count_request, forge_test_count_reply>(
            kFail, "fail", [](const auto&, auto&) { return Status::error(module_id::app, ERANGE, "no"); });
        r->add_push<forge_test_level_report>(kLevel, "level");
        return r;
    }();
    return *registry;
}

std::string frame(std::uint8_t flags, std::uint16_t id, std::uint16_t seq, std::string payload) {
    std::string out(c2::header_size + payload.size(), '\0');
    std::size_t n = 0;
    c2::encode_message(c2::make_header(flags, id, seq, static_cast<std::uint16_t>(payload.size())),
                       {reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()},
                       {reinterpret_cast<std::uint8_t*>(out.data()), out.size()}, n);
    return out;
}

}  // namespace

const std::vector<std::string>& seeds() {
    static const std::vector<std::string> s = {
        frame(c2::flag::req | c2::flag::proto, kEcho, 1, std::string("\x08\x2a\x12\x02hi", 6)),  // token 42, "hi"
        frame(c2::flag::req | c2::flag::proto, kCount, 2, std::string("\x08\x05", 2)),           // to 5
        frame(c2::flag::req, kFail, 3, ""),
        frame(c2::flag::push, kLevel, 4, std::string("\x08\x7f", 2)),
        frame(c2::flag::rsp | c2::flag::more, kCount, 5, std::string("\x08\x01", 2)),
        frame(c2::flag::nack, kEcho, 6, std::string("\x80\x40\x00\x16", 4)),
        frame(c2::flag::req, 0x7777, 7, ""),  // unknown command
    };
    return s;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> in(data, size);

    c2::Message m;
    const bool decoded = c2::decode_message(in, m).ok();
    if (decoded) {
        if (m.payload.size() != m.header.length) fuzz_fail("payload size is not the header's length");
        std::vector<std::uint8_t> again(size);
        std::size_t n = 0;
        if (!c2::encode_message(m.header, m.payload, again, n).ok() || n != size ||
            std::memcmp(again.data(), data, size) != 0) {
            fuzz_fail("a decoded frame does not encode back to the same bytes");
        }
        Status carried;
        if (c2::kind_of(m.header.flags) == c2::Kind::nack) (void)c2::nack_status(m.payload, carried);
        (void)c2::flags_to_string(m.header.flags);
    }

    std::size_t answers = 0;
    const auto kind = tmtc::Dispatcher(commands()).handle(in, [&](std::span<const std::uint8_t> out) {
        ++answers;
        c2::Message a;
        if (!c2::decode_message(out, a).ok()) fuzz_fail("an answer is not a valid frame");
        if (a.header.cmd_id != m.header.cmd_id || a.header.seq != m.header.seq) {
            fuzz_fail("an answer has another command or sequence number");
        }
        const auto k = c2::kind_of(a.header.flags);
        if (k != c2::Kind::reply && k != c2::Kind::ack && k != c2::Kind::nack) fuzz_fail("an answer is not a reply");
    });
    if (!decoded && kind != c2::Kind::unknown) fuzz_fail("a frame that does not decode was dispatched");
    if (answers > 0 && kind != c2::Kind::request) fuzz_fail("something other than a request was answered");
    return 0;
}
