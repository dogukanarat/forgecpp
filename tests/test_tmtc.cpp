#include <forge/error.hpp>
#include <forge/testing.hpp>
#include <forge/tmtc.hpp>

#include "forge_test/commands.forge.hpp"

#include <cerrno>
#include <stdexcept>
#include <vector>

namespace c2 = forge::c2;
namespace pb = forge::pb;
namespace tmtc = forge::tmtc;
using forge::json;
using forge::Status;

namespace {

constexpr std::uint16_t kEcho = 0x0101;
constexpr std::uint16_t kCount = 0x0102;
constexpr std::uint16_t kFail = 0x0103;
constexpr std::uint16_t kQuiet = 0x0104;
constexpr std::uint16_t kLevel = 0x0181;

using Bytes = std::vector<std::uint8_t>;

struct Fixture {
    tmtc::Registry commands;
    std::vector<Bytes> sent;

    Fixture() {
        commands.add<forge_test_echo_request, forge_test_echo_reply>(
            kEcho, "echo", [](const forge_test_echo_request& req, forge_test_echo_reply& rsp) {
                rsp.token = req.token;
                return pb::assign(rsp.text, pb::view(req.text));
            });
        // Answers 1, 2, ... `to` in one reply each.
        commands.add_parts<forge_test_count_request, forge_test_count_reply>(
            kCount, "count", [](const forge_test_count_request& req, forge_test_count_reply& rsp, std::uint32_t& more) {
                rsp.value = more + 1;
                more = rsp.value < req.to ? rsp.value : 0;
                return Status();
            });
        commands.add<forge_test_count_request, forge_test_count_reply>(
            kFail, "fail", [](const forge_test_count_request&, forge_test_count_reply&) {
                return Status::error(forge::module_id::app, ERANGE, "too far");
            });
        commands.add<forge_test_count_request, forge_test_count_reply>(
            kQuiet, "quiet", [](const forge_test_count_request&, forge_test_count_reply&) { return Status(); });
        commands.add_push<forge_test_level_report>(kLevel, "level");
    }

    template <class Msg>
    Bytes request(std::uint16_t id, std::uint16_t seq, const Msg& msg) {
        std::array<std::uint8_t, 64> payload{};
        std::size_t n = 0;
        REQUIRE(pb::encode(msg, payload, n).ok());
        Bytes out(c2::header_size + n);
        std::size_t len = 0;
        REQUIRE(c2::encode_message(c2::make_header(c2::flag::req, id, seq, static_cast<std::uint16_t>(n)),
                                   std::span(payload).first(n), out, len)
                    .ok());
        return out;
    }

    c2::Kind dispatch(const Bytes& message) {
        sent.clear();
        tmtc::Dispatcher d(commands);
        return d.handle(message, [&](std::span<const std::uint8_t> m) { sent.emplace_back(m.begin(), m.end()); });
    }

    c2::Message answer(std::size_t i = 0) {
        REQUIRE(i < sent.size());
        c2::Message m;
        REQUIRE(c2::decode_message(sent[i], m).ok());
        return m;
    }

    Status refusal(std::size_t i = 0) {
        const auto m = answer(i);
        REQUIRE(m.header.flags == c2::flag::nack);
        Status s;
        REQUIRE(c2::nack_status(m.payload, s).ok());
        return s;
    }
};

}  // namespace

TEST_CASE("tmtc: commands are found by number and by name") {
    Fixture f;
    REQUIRE(f.commands.find(kEcho) != nullptr);
    CHECK(f.commands.find(kEcho)->name == "echo");
    CHECK(f.commands.find("count")->id == kCount);
    CHECK(f.commands.find(0x0999) == nullptr);
    CHECK(f.commands.find("nothing") == nullptr);
    CHECK(f.commands.find(kLevel)->is_push());
    CHECK(std::string(f.commands.find(kLevel)->rsp->name) == "forge_test.level_report");

    std::vector<std::uint16_t> ids;
    for (const auto* c : f.commands.list()) ids.push_back(c->id);
    CHECK(ids == std::vector<std::uint16_t>{kEcho, kCount, kFail, kQuiet, kLevel});

    f.commands.remove(kQuiet);
    CHECK(f.commands.find(kQuiet) == nullptr);
    CHECK(f.commands.find("quiet") == nullptr);
}

TEST_CASE("tmtc: a number or a name used twice is refused at once") {
    Fixture f;
    const auto add = [&](std::uint16_t id, const char* name) {
        f.commands.add<forge_test_count_request, forge_test_count_reply>(
            id, name, [](const forge_test_count_request&, forge_test_count_reply&) { return Status(); });
    };
    CHECK_THROWS_WITH_AS(add(kEcho, "other"), "tmtc: number 0x0101 is already 'echo'", std::logic_error);
    CHECK_THROWS_WITH_AS(add(0x0200, "echo"), "tmtc: name 'echo' is already used", std::logic_error);
    CHECK_THROWS_AS(add(0, "zero"), std::logic_error);
    CHECK(f.commands.find("other") == nullptr);
}

TEST_CASE("tmtc: a request gets its reply, with its number and sequence") {
    Fixture f;
    forge_test_echo_request req{};
    req.token = 77;
    REQUIRE(pb::assign(req.text, "merhaba").ok());
    CHECK(f.dispatch(f.request(kEcho, 42, req)) == c2::Kind::request);
    REQUIRE(f.sent.size() == 1);
    const auto m = f.answer();
    CHECK(m.header.flags == c2::flag::rsp);
    CHECK(m.header.cmd_id == kEcho);
    CHECK(m.header.seq == 42);
    forge_test_echo_reply rsp{};
    REQUIRE(pb::decode(m.payload, rsp).ok());
    CHECK(rsp.token == 77);
    CHECK(pb::view(rsp.text) == "merhaba");
}

TEST_CASE("tmtc: an empty reply is an ACK") {
    Fixture f;
    f.dispatch(f.request(kQuiet, 1, forge_test_count_request{}));
    REQUIRE(f.sent.size() == 1);
    CHECK(f.answer().header.flags == c2::flag::ack);
    CHECK(f.answer().payload.empty());
}

TEST_CASE("tmtc: refusals carry the status") {
    Fixture f;
    f.dispatch(f.request(0x0999, 3, forge_test_count_request{}));
    CHECK(f.refusal().errno_code() == ENOENT);
    CHECK(f.answer().header.seq == 3);

    f.dispatch(f.request(kLevel, 4, forge_test_count_request{}));  // a report, not a command
    CHECK(f.refusal().errno_code() == ENOSYS);

    f.dispatch(f.request(kFail, 5, forge_test_count_request{}));  // the handler's own status
    CHECK(f.refusal().module() == forge::module_id::app);
    CHECK(f.refusal().errno_code() == ERANGE);

    // A payload that is not an echo_request.
    Bytes bad = f.request(kEcho, 6, forge_test_count_request{});
    bad.push_back(0xFF);
    bad[7] = static_cast<std::uint8_t>(bad.size() - c2::header_size);
    f.dispatch(bad);
    CHECK(f.refusal().errno_code() == EBADMSG);
}

TEST_CASE("tmtc: an answer in parts") {
    Fixture f;
    forge_test_count_request req{};
    req.to = 3;
    f.dispatch(f.request(kCount, 9, req));
    REQUIRE(f.sent.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        const auto m = f.answer(i);
        CHECK(m.header.seq == 9);
        CHECK(m.header.flags == (i < 2 ? (c2::flag::rsp | c2::flag::more) : c2::flag::rsp));
        forge_test_count_reply rsp{};
        REQUIRE(pb::decode(m.payload, rsp).ok());
        CHECK(rsp.value == i + 1);
    }
}

TEST_CASE("tmtc: only requests are answered") {
    Fixture f;
    auto reply = f.request(kEcho, 1, forge_test_echo_request{});
    reply[1] = c2::flag::rsp;
    CHECK(f.dispatch(reply) == c2::Kind::reply);
    CHECK(f.sent.empty());

    const Bytes garbage{0x02, 0x01, 0x00};
    CHECK(f.dispatch(garbage) == c2::Kind::unknown);
    CHECK(f.sent.empty());
}

TEST_CASE("tmtc: the same handler answers JSON") {
    Fixture f;
    const auto* echo = f.commands.find("echo");
    CHECK(echo->run_json(json{{"token", 5}, {"text", "hi"}}) == json{{"token", 5}, {"text", "hi"}});
    CHECK(echo->run_json(nullptr) == json{{"token", 0}, {"text", ""}});

    CHECK(f.commands.find("count")->run_json(json{{"to", 2}}) == json::array({{{"value", 1}}, {{"value", 2}}}));

    try {
        f.commands.find("fail")->run_json(json::object());
        FAIL("no error");
    } catch (const forge::Error& e) {
        CHECK(e.status() == 400);  // ERANGE
        CHECK(std::string(e.what()) == "app: too far (ERANGE)");
    }
    CHECK_THROWS_WITH_AS(echo->run_json(json{{"text", std::string(20, 'x')}}), "text: longer than 15 bytes",
                         forge::Error);
}
