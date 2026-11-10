#include <forge/error.hpp>
#include <forge/pb.hpp>
#include <forge/testing.hpp>

#include "forge_test/all_types.forge.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <vector>

using forge::json;
namespace pb = forge::pb;

namespace {

std::vector<std::uint8_t> from_hex(const std::string& hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// Same JSON, with float fields compared as float32 (Python prints the shortest
// text that reads back as the same float; forgecpp prints the double).
bool same(const json& a, const json& b) {
    if (a.is_number() && b.is_number()) {
        const double x = a.get<double>(), y = b.get<double>();
        return x == y || std::abs(x - y) <= 1e-6 * std::max(std::abs(x), std::abs(y));
    }
    if (a.type() != b.type() || a.size() != b.size()) return false;
    if (a.is_object()) {
        for (const auto& [k, v] : a.items()) {
            if (!b.contains(k) || !same(v, b[k])) return false;
        }
        return true;
    }
    if (a.is_array()) {
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!same(a[i], b[i])) return false;
        }
        return true;
    }
    return a == b;
}

json rejected(const json& j) {
    try {
        pb::from_json<forge_test_all_types>(j);
    } catch (const forge::Error& e) {
        CHECK(e.status() == 400);
        return e.what();
    }
    return nullptr;
}

}  // namespace

TEST_CASE("pb: a message survives encode and decode") {
    forge_test_all_types m{};
    m.i32 = -5;
    m.u64 = 1ULL << 63;
    REQUIRE(pb::assign(m.text, "çalışma").ok());
    m.has_one = true;
    m.one.a = 42;
    const std::array<std::int32_t, 3> list{1, -2, 3};
    REQUIRE(pb::assign(m.list, m.list_count, list).ok());
    m.which_choice = forge_test_all_types_name_tag;
    REQUIRE(pb::assign(m.choice.name, "x").ok());

    std::array<std::uint8_t, forge_test_all_types_size> buf{};
    std::size_t n = 0;
    REQUIRE(pb::encode(m, buf, n).ok());
    forge_test_all_types back{};
    REQUIRE(pb::decode(std::span(buf).first(n), back).ok());
    CHECK(back.i32 == -5);
    CHECK(back.u64 == (1ULL << 63));
    CHECK(pb::view(back.text) == "çalışma");
    CHECK(back.has_one);
    CHECK(back.one.a == 42);
    CHECK(back.list_count == 3);
    CHECK(back.list[1] == -2);
    CHECK(back.which_choice == forge_test_all_types_name_tag);
    CHECK(pb::view(back.choice.name) == "x");
}

TEST_CASE("pb: encode and decode report errors") {
    forge_test_all_types m{};
    REQUIRE(pb::assign(m.text, "sixteen chars...").ok());
    std::array<std::uint8_t, 4> small{};
    std::size_t n = 0;
    const auto s = pb::encode(m, small, n);
    CHECK(s.module() == forge::module_id::pb);
    CHECK(s.errno_code() == EMSGSIZE);

    const std::array<std::uint8_t, 3> garbage{0x0A, 0x7F, 0x01};  // field 1 as bytes, 127 long, 1 there
    CHECK(pb::decode(garbage, m).errno_code() == EBADMSG);
}

TEST_CASE("pb: bounded copies refuse rather than cut") {
    char label[5] = "abc";
    CHECK(pb::assign(label, "abcd").ok());
    CHECK(pb::view(label) == "abcd");
    CHECK(pb::assign(label, "abcde").errno_code() == ENAMETOOLONG);
    CHECK(pb::view(label) == "abcd");  // unchanged

    std::int32_t values[2] = {};
    pb_size_t count = 0;
    const std::vector<std::int32_t> three{1, 2, 3};
    CHECK(pb::assign(values, count, three).errno_code() == E2BIG);
    CHECK(count == 0);
    CHECK(pb::assign(values, count, std::vector<std::int32_t>{7}).ok());
    CHECK(count == 1);
}

TEST_CASE("pb: JSON names, kinds and presence") {
    forge_test_all_types m{};
    m.i64 = -9007199254740993LL;  // 2^53 + 1: lost in a JSON number
    m.colour = forge_test_color_blue;
    m.data.size = 2;
    m.data.bytes[0] = 0xFF;
    m.data.bytes[1] = 0x00;
    m.fl = 0.5f;
    m.db = INFINITY;
    const auto j = pb::to_json(m);
    CHECK(j["i64"] == "-9007199254740993");
    CHECK(j["i32"] == 0);  // every field is written, also the defaults
    CHECK(j["colour"] == "blue");
    CHECK(j["data"] == "/wA=");
    CHECK(j["fl"] == 0.5);
    CHECK(j["db"] == "Infinity");
    CHECK_FALSE(j.contains("one"));     // a message field with no value
    CHECK_FALSE(j.contains("number"));  // no oneof member set
    CHECK_FALSE(j.contains("maybe"));   // optional, not set
    CHECK(j["list"] == json::array());

    const auto back = pb::from_json<forge_test_all_types>(j);
    CHECK(back.i64 == m.i64);
    CHECK(back.colour == forge_test_color_blue);
    CHECK(back.data.size == 2);
    CHECK(std::isinf(back.db));

    // Input: lowerCamelCase names, numbers for enums, numbers or text for integers.
    const auto in = pb::from_json<forge_test_all_types>(json{{"u64", 7}, {"colour", 2}, {"i32", "-3"}, {"sf32", 4.0}});
    CHECK(in.u64 == 7);
    CHECK(in.colour == forge_test_color_green);
    CHECK(in.i32 == -3);
    CHECK(in.sf32 == 4);
}

TEST_CASE("pb: a message's schema, for clients that build one") {
    const auto s = pb::schema(*pb::type_of<forge_test_all_types>);
    CHECK(s["name"] == "forge_test.all_types");
    const auto field = [&](const std::string& name) {
        for (const auto& f : s["fields"]) {
            if (f["name"] == name) return f;
        }
        FAIL("no field " << name);
        return json();
    };
    CHECK(field("i64")["kind"] == "int");
    CHECK(field("i64")["bits"] == 64);
    CHECK(field("u32")["kind"] == "uint");
    CHECK(field("text")["max_length"] == 16);  // max_size 17 counts the NUL
    CHECK(field("data")["max_size"] == 8);
    CHECK(field("key")["max_size"] == 4);
    CHECK(field("list")["repeated"] == true);
    CHECK(field("list")["max_count"] == 4);
    CHECK(field("colour")["values"] == json::array({"color_none", "red", "green", "blue"}));
    CHECK(field("one")["fields"][1]["name"] == "s");
    CHECK(field("one")["fields"][1]["max_length"] == 8);
}

TEST_CASE("pb: wrong JSON is refused with the path of the field") {
    CHECK(rejected(json{{"nope", 1}}) == "nope: not a field of forge_test.all_types");
    CHECK(rejected(json{{"text", std::string(17, 'x')}}) == "text: longer than 16 bytes");
    CHECK(rejected(json{{"list", {1, 2, 3, 4, 5}}}) == "list: more than 4 values");
    CHECK(rejected(json{{"many", {{{"s", "123456789"}}}}}) == "many[0].s: longer than 8 bytes");
    CHECK(rejected(json{{"i32", 2147483648LL}}) == "i32: out of range");
    CHECK(rejected(json{{"u32", -1}}) == "u32: out of range");
    CHECK(rejected(json{{"i32", 1.5}}) == "i32: not an integer");
    CHECK(rejected(json{{"colour", "purple"}}) == "colour: unknown value 'purple'");
    CHECK(rejected(json{{"key", "AQID"}}) == "key: 3 bytes, the field holds 4");
    CHECK(rejected(json{{"data", "%%%"}}) == "data: not base64 text");
    CHECK(rejected(json{{"number", 1}, {"name", "a"}}) == "number: another member of its oneof is already set");
    CHECK(rejected(json{{"b", 1}}) == "b: not true or false");
    CHECK(rejected(json::array()) == "forge_test.all_types: not an object");
    CHECK(rejected(json{{"text", nullptr}}) == nullptr);  // null means "not set"
}

#ifdef FORGE_PB_GOLDEN
TEST_CASE("pb: the same JSON and bytes as Google's protobuf for Python") {
    std::ifstream in(FORGE_PB_GOLDEN);
    REQUIRE(in);
    std::string line;
    int cases = 0;
    while (std::getline(in, line)) {
        const auto golden = json::parse(line);
        const auto bytes = from_hex(golden["hex"].get<std::string>());
        INFO(line);

        // JSON -> struct -> bytes: the bytes Python wrote.
        const auto m = pb::from_json<forge_test_all_types>(golden["json"]);
        std::array<std::uint8_t, forge_test_all_types_size> buf{};
        std::size_t n = 0;
        REQUIRE(pb::encode(m, buf, n).ok());
        REQUIRE(std::vector<std::uint8_t>(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)) == bytes);

        // bytes -> struct -> JSON: the JSON Python wrote.
        forge_test_all_types back{};
        REQUIRE(pb::decode(bytes, back).ok());
        REQUIRE(same(pb::to_json(back), golden["json"]));
        ++cases;
    }
    CHECK(cases == 500);
}
#endif
