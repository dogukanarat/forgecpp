// Messages from outside: protobuf bytes from a link and JSON from the web.
//
// The first byte picks the way in. Properties: bytes that decode encode again
// within the message's size, and JSON -> struct -> JSON -> struct gives the
// same struct; bad JSON is refused with forge::Error, nothing else.
#include "forge/error.hpp"
#include "forge/pb.hpp"
#include "forge_test/all_types.forge.hpp"
#include "fuzz.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

using forge::json;
namespace pb = forge::pb;

namespace {

std::array<std::uint8_t, forge_test_all_types_size> encoded(const forge_test_all_types& m, std::size_t& n) {
    std::array<std::uint8_t, forge_test_all_types_size> out{};
    if (!pb::encode(m, out, n).ok()) fuzz_fail("a message that decoded does not encode");
    if (n != pb::encoded_size(*pb::type_of<forge_test_all_types>, &m)) fuzz_fail("encoded_size is wrong");
    return out;
}

// JSON has one NaN: its sign and payload bits do not survive, and need not.
forge_test_all_types plain_nan(forge_test_all_types m) {
    if (std::isnan(m.fl)) m.fl = std::numeric_limits<float>::quiet_NaN();
    if (std::isnan(m.db)) m.db = std::numeric_limits<double>::quiet_NaN();
    return m;
}

// The same message after a trip through JSON.
void json_round_trip(const forge_test_all_types& original) {
    const auto m = plain_nan(original);
    const json j = pb::to_json(m);
    forge_test_all_types back{};
    try {
        back = pb::from_json<forge_test_all_types>(j);
    } catch (const forge::Error&) {
        fuzz_fail("to_json gave JSON that from_json refuses");
    }
    std::size_t n1 = 0, n2 = 0;
    const auto a = encoded(m, n1);
    const auto b = encoded(back, n2);
    if (n1 != n2 || std::memcmp(a.data(), b.data(), n1) != 0) fuzz_fail("JSON round trip changed the message");
}

}  // namespace

const std::vector<std::string>& seeds() {
    static const std::vector<std::string> s = [] {
        forge_test_all_types m{};
        m.b = true;
        m.i32 = -5;
        m.i64 = -1234567890123;
        m.u64 = 18446744073709551615ull;
        m.s32 = -7;
        m.fl = 1.5f;
        m.db = -2.25;
        std::strcpy(m.text, "hello");
        m.data.size = 3;
        m.colour = forge_test_color_blue;
        m.has_one = true;
        m.one.a = 9;
        m.list_count = 2;
        m.list[0] = 1;
        m.list[1] = -1;
        std::array<std::uint8_t, forge_test_all_types_size> buf{};
        std::size_t n = 0;
        (void)pb::encode(m, buf, n);
        return std::vector<std::string>{
            std::string(1, '\0') + std::string(reinterpret_cast<const char*>(buf.data()), n),
            std::string(1, '\1') + pb::to_json(m).dump(),
            std::string(1, '\1') +
                R"({"text": "abcdefghijklmnopqrstuvwxyz", "list": [1, 2, 3, 4, 5], "colour": "nope"})",
        };
    }();
    return s;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    const std::span<const std::uint8_t> in(data + 1, size - 1);
    if (data[0] % 2 == 0) {  // bytes from a link
        forge_test_all_types m{};
        if (!pb::decode(in, m).ok()) return 0;
        std::size_t n = 0;
        (void)encoded(m, n);
        json_round_trip(m);
    } else {  // JSON from the web
        const json j = json::parse(in.begin(), in.end(), nullptr, false);
        if (j.is_discarded()) return 0;
        try {
            json_round_trip(pb::from_json<forge_test_all_types>(j));
        } catch (const forge::Error&) {
            // refused with a reason: fine
        }
    }
    return 0;
}
