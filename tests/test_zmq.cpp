// ZeroMQ: framing, streams, request/reply, the command endpoint and the
// events of an application.
#include <forge/control.hpp>
#include <forge/forge.hpp>
#include <forge/messages.forge.hpp>
#include <forge/testing.hpp>
#include <forge/trace.hpp>
#include <forge/zmq.hpp>

#include <zmq_addon.hpp>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using forge::json;
using namespace std::chrono_literals;
namespace fz = forge::zmq;
namespace pb = forge::pb;

namespace {

forge_log_record record(const std::string& text) {
    forge_log_record r{};
    REQUIRE(pb::assign(r.message, text).ok());
    REQUIRE(pb::assign(r.level, "info").ok());
    return r;
}

std::vector<std::string> one(std::string frame) { return {std::move(frame)}; }

// One raw REQ exchange; no frames on timeout.
std::vector<std::string> req_exchange(const std::string& endpoint, const std::string& body,
                                      std::chrono::milliseconds timeout = 3s) {
    ::zmq::socket_t s(fz::context(), ::zmq::socket_type::req);
    s.set(::zmq::sockopt::linger, 0);
    s.connect(endpoint);
    s.send(::zmq::buffer(body), ::zmq::send_flags::none);
    ::zmq::pollitem_t item{s.handle(), 0, ZMQ_POLLIN, 0};
    if (::zmq::poll(&item, 1, timeout) <= 0) return {};
    std::vector<::zmq::message_t> parts;
    (void)::zmq::recv_multipart(s, std::back_inserter(parts));
    std::vector<std::string> out;
    for (const auto& p : parts) out.push_back(p.to_string());
    return out;
}

}  // namespace

TEST_CASE("zmq: framing - header, typed body, blob and topic filtering") {
    fz::set_source("zmq-test");
    fz::Sender pub(fz::Sender::Kind::pub, "tcp://127.0.0.1:*");
    REQUIRE(pub.endpoint().rfind("tcp://127.0.0.1:", 0) == 0);

    std::mutex m;
    std::vector<fz::Message> got;
    fz::Receiver::Options opts;
    opts.topics = {"iq"};
    fz::Receiver sub(
        fz::Receiver::Kind::sub, pub.endpoint(),
        [&](fz::Message&& msg) {
            std::lock_guard lock(m);
            got.push_back(std::move(msg));
        },
        opts);
    // Subscriptions propagate asynchronously ("slow joiner"): wait for them.
    REQUIRE(forge::testing::wait_until([&] { return pub.subscriptions() == 1; }));

    const std::vector<std::int16_t> samples{1, -1, 32767, -32768};
    CHECK(pub.publish("status", record("ignored")));
    CHECK(pub.publish("iq", record("first"), samples.data(), samples.size() * sizeof(std::int16_t)));
    CHECK(pub.publish("iq", record("second")));
    REQUIRE(forge::testing::wait_until([&] {
        std::lock_guard lock(m);
        return got.size() == 2;
    }));

    std::lock_guard lock(m);
    const auto& a = got[0];
    CHECK(a.topic == "iq");
    CHECK(pb::view(a.header.source) == "zmq-test");
    CHECK(pb::view(a.header.type) == "forge.log_record");
    CHECK(a.header.seq == 1);
    CHECK(got[1].header.seq == 2);
    CHECK(a.header.time_unix_ns > 0);
    CHECK(pb::view(a.as<forge_log_record>().message) == "first");
    CHECK_THROWS_AS(a.as<forge_alarm>(), fz::Error);  // wrong type
    REQUIRE(a.blob.size() == 8);
    std::int16_t back[4];
    std::memcpy(back, a.blob.data(), sizeof(back));
    CHECK(back[2] == 32767);
    CHECK(back[3] == -32768);
    CHECK(got[1].blob.empty());
}

TEST_CASE("zmq: the sender's trace context travels in the header") {
    fz::Sender pub(fz::Sender::Kind::pub, "tcp://127.0.0.1:*");
    std::atomic<bool> same{false}, received{false};
    const auto ctx = forge::trace::new_trace();
    fz::Receiver sub(fz::Receiver::Kind::sub, pub.endpoint(), [&](fz::Message&&) {
        same = forge::trace::current().trace_hex() == ctx.trace_hex();
        received = true;
    });
    REQUIRE(forge::testing::wait_until([&] { return pub.subscriptions() == 1; }));
    {
        forge::trace::ContextScope scope(ctx);
        pub.publish("t", record("traced"));
    }
    REQUIRE(forge::testing::wait_until([&] { return received.load(); }));
    CHECK(same);
}

TEST_CASE("zmq: publisher streams - subscribers per topic, rate limit, latest-only, zero copy, loss count") {
    fz::Publisher pub("tcp://127.0.0.1:*");
    auto& fast = pub.add<forge_log_record>("fast");
    auto& slow = pub.add<forge_log_record>("slow", {"latest", 5});
    CHECK_FALSE(fast.wanted());  // nobody listens yet
    CHECK_THROWS_AS(pub.add<forge_alarm>("fast"), std::logic_error);  // a stream keeps its type
    CHECK_THROWS_AS(fast.publish(forge_alarm{}), std::logic_error);

    std::mutex m;
    std::vector<fz::Message> got;
    fz::Receiver::Options opts;
    opts.topics = {"fast"};
    fz::Receiver sub(
        fz::Receiver::Kind::sub, pub.endpoint(),
        [&](fz::Message&& msg) {
            std::lock_guard lock(m);
            got.push_back(std::move(msg));
        },
        opts);
    REQUIRE(forge::testing::wait_until([&] { return fast.subscribers() == 1; }));
    CHECK(slow.subscribers() == 0);
    CHECK(fast.wanted());
    CHECK_FALSE(slow.wanted());

    // Zero copy: the blob owner lives until ZeroMQ has sent it.
    auto blob = std::make_shared<std::vector<std::int16_t>>(1024, 7);
    std::weak_ptr<std::vector<std::int16_t>> weak = blob;
    CHECK(fast.publish(record("with blob"), blob, blob->data(), blob->size() * 2));
    blob.reset();
    for (int i = 0; i < 9; ++i) fast.publish(record("n" + std::to_string(i)));
    REQUIRE(forge::testing::wait_until([&] {
        std::lock_guard lock(m);
        return got.size() == 10;
    }));
    CHECK(forge::testing::wait_until([&] { return weak.expired(); }));
    {
        std::lock_guard lock(m);
        CHECK(got[0].blob.size() == 2048);
        CHECK(got[9].header.seq == 10);
    }
    CHECK(fast.info().sent == 10);
    CHECK(sub.lost() == 0);

    // Rate limit: at most 5 per second -> wanted() is false right after a true.
    fz::Receiver::Options o2;
    o2.topics = {"slow"};
    std::atomic<int> slow_got{0};
    fz::Receiver sub2(fz::Receiver::Kind::sub, pub.endpoint(), [&](fz::Message&&) { ++slow_got; }, o2);
    REQUIRE(forge::testing::wait_until([&] { return slow.subscribers() == 1; }));
    CHECK(slow.wanted());
    CHECK_FALSE(slow.wanted());
    // latest-only: a burst collapses to the newest messages, never a backlog.
    for (int i = 0; i < 100; ++i) slow.publish(record("s" + std::to_string(i)));
    REQUIRE(forge::testing::wait_until([&] { return slow_got > 0; }));
    std::this_thread::sleep_for(100ms);
    CHECK(slow_got < 100);
    CHECK(slow.info().dropped > 0);

    const auto list = pub.list();
    REQUIRE(list.size() == 2);
    CHECK(list[0].name == "fast");
    CHECK(list[0].type == "forge.log_record");
}

TEST_CASE("zmq: a command client that recovers from timeouts") {
    forge::tmtc::Registry commands;
    commands.add<forge_ping_request, forge_ping_reply>(forge_msg_id_ping, "ping", [](const auto& req, auto& rsp) {
        rsp.token = req.token + 1;
        return forge::Status();
    });
    const auto serve = [&](std::string_view req) { return fz::answer(commands, req); };
    auto server = std::make_unique<fz::Server>("tcp://127.0.0.1:*", serve);
    const auto endpoint = server->endpoint();
    fz::Client client(endpoint, 2s);
    forge_ping_request req{};
    req.token = 41;
    forge_ping_reply rsp{};
    REQUIRE(client.call(forge_msg_id_ping, req, rsp).ok());
    CHECK(rsp.token == 42);

    server.reset();
    client.timeout(200ms);
    CHECK(client.call(forge_msg_id_ping, req, rsp).errno_code() == ETIMEDOUT);
    fz::Server again(endpoint, serve);
    client.timeout(2s);
    REQUIRE(client.call(forge_msg_id_ping, req, rsp).ok());  // the same client works again
    CHECK(rsp.token == 42);
    CHECK(client.call(forge_msg_id_info, forge_info_request{}, forge_info_reply{} = forge_info_reply{}).errno_code() ==
          ENOENT);
}

TEST_CASE("zmq: a slow request does not hold up the others; replies go to the client that asked") {
    std::atomic<int> slow_running{0};
    fz::ServerOptions options;
    options.workers = 2;
    fz::Server server(
        "tcp://127.0.0.1:*",
        [&](std::string_view req) {
            if (req == "slow") {
                ++slow_running;
                std::this_thread::sleep_for(800ms);
                return one("slow done");
            }
            return std::vector<std::string>{"re:" + std::string(req), "part 2"};
        },
        options);
    CHECK(server.workers() == 2);

    std::vector<std::string> slow_reply;
    std::thread slow([&] { slow_reply = req_exchange(server.endpoint(), "slow"); });
    while (slow_running == 0) std::this_thread::sleep_for(1ms);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(req_exchange(server.endpoint(), "ping") == std::vector<std::string>{"re:ping", "part 2"});
    CHECK(std::chrono::steady_clock::now() - t0 < 400ms);

    // Many clients at once: each gets its own answer.
    std::vector<std::thread> clients;
    std::atomic<int> right{0};
    for (int i = 0; i < 16; ++i) {
        clients.emplace_back([&, i] {
            const auto r = req_exchange(server.endpoint(), std::to_string(i));
            if (!r.empty() && r[0] == "re:" + std::to_string(i)) ++right;
        });
    }
    for (auto& c : clients) c.join();
    CHECK(right == 16);
    slow.join();
    CHECK(slow_reply == one("slow done"));
    CHECK(server.requests() == 18);
}

TEST_CASE("zmq: requests that waited too long for a worker are refused, not handled") {
    std::atomic<int> handled{0};
    fz::ServerOptions options;
    options.workers = 1;
    options.max_queue_wait = 100ms;
    options.busy = [](std::string_view req) { return one("busy:" + std::string(req)); };
    fz::Server server(
        "tcp://127.0.0.1:*",
        [&](std::string_view req) {
            ++handled;
            if (req == "slow") std::this_thread::sleep_for(400ms);
            return one("ok");
        },
        options);
    std::thread slow([&] { CHECK(req_exchange(server.endpoint(), "slow") == one("ok")); });
    while (handled == 0) std::this_thread::sleep_for(1ms);
    CHECK(req_exchange(server.endpoint(), "late") == one("busy:late"));
    slow.join();
    CHECK(handled == 1);
    CHECK(server.rejected() == 1);
}

namespace {
struct ControlledApp : forge::Application {
    ControlledApp() : Application({"zmqapp", "3.0.0"}) {}
    double freq = 100.0;
    int presses = 0;

protected:
    void setup_web(forge::web::Dashboard& ui) override {
        auto& g = ui.group("Radio");
        g.number("freq", "Frequency", freq).range(70, 6000);
        g.button("cal", "Calibrate", [this] { ++presses; });
        g.command("scan", "Scan", {forge::web::number_param("from", 100), forge::web::number_param("to")},
                  [](const json& a) { return json{{"width", a["to"].get<double>() - a["from"].get<double>()}}; });
    }
};

template <std::size_t N>
void set(char (&dst)[N], const std::string& text) {
    REQUIRE(pb::assign(dst, text).ok());
}
}  // namespace

TEST_CASE("zmq: an application's command endpoint and its events") {
    forge::testing::AppRunner<ControlledApp> r(
        {"-D", "forge.control=tcp://127.0.0.1:*", "-D", "forge.events=tcp://127.0.0.1:*"});
    const auto ep = r->endpoints();
    REQUIRE(ep.count("control"));
    REQUIRE(ep.count("events"));

    // Events: state changes and the audit trail.
    std::mutex m;
    std::vector<fz::Message> events;
    fz::Receiver sub(fz::Receiver::Kind::sub, ep.at("events"), [&](fz::Message&& msg) {
        std::lock_guard lock(m);
        events.push_back(std::move(msg));
    });
    REQUIRE(forge::testing::wait_until([&] {
        // until the subscription is in place (the first published event arrives)
        r->alarms().raise("probe", forge::Severity::info, "probe");
        r->alarms().clear("probe");
        std::lock_guard lock(m);
        return !events.empty();
    }));

    fz::Client c(ep.at("control"));
    forge_info_reply info{};
    REQUIRE(c.call(forge_msg_id_info, forge_info_request{}, info).ok());
    CHECK(pb::view(info.name) == "zmqapp");
    CHECK(pb::view(info.version) == "3.0.0");
    bool listed = false;
    for (pb_size_t i = 0; i < info.endpoints_count; ++i) {
        listed = listed ||
                 (pb::view(info.endpoints[i].name) == "control" && pb::view(info.endpoints[i].url) == ep.at("control"));
    }
    CHECK(listed);

    forge_set_request set_req{};
    set(set_req.id, "freq");
    set(set_req.value, "2400");
    forge_set_reply set_rsp{};
    REQUIRE(c.call(forge_msg_id_set, set_req, set_rsp).ok());
    CHECK(json::parse(set_rsp.value) == 2400);

    forge_get_request get_req{};
    set(get_req.id, "freq");
    forge_get_reply get_rsp{};
    REQUIRE(c.call(forge_msg_id_get, get_req, get_rsp).ok());
    CHECK(json::parse(get_rsp.value) == 2400);

    forge_press_request press{};
    set(press.id, "cal");
    forge_press_reply pressed{};
    REQUIRE(c.call(forge_msg_id_press, press, pressed).ok());

    forge_call_request call{};
    set(call.id, "scan");
    set(call.args, R"({"to": 600})");
    forge_call_reply result{};
    REQUIRE(c.call(forge_msg_id_call, call, result).ok());
    CHECK(json::parse(result.result)["width"] == 500);

    // Refusals carry the errno of the reason.
    set(set_req.value, "9999");
    CHECK(c.call(forge_msg_id_set, set_req, set_rsp).errno_code() == EINVAL);  // out of range
    set(call.args, "{}");
    CHECK(c.call(forge_msg_id_call, call, result).errno_code() == EINVAL);  // "to" is required
    set(get_req.id, "nothing");
    CHECK(c.call(forge_msg_id_get, get_req, get_rsp).errno_code() == ENOENT);

    // Lists come in parts.
    for (int i = 0; i < 20; ++i) r->alarms().raise("a" + std::to_string(i), forge::Severity::warning, "alarm");
    int alarms = 0, parts = 0;
    REQUIRE(c.call_parts<forge_alarms_request, forge_alarms_reply>(forge_msg_id_alarms, {},
                                                                   [&](const forge_alarms_reply& page) {
                                                                       ++parts;
                                                                       alarms += page.alarms_count;
                                                                   })
                .ok());
    CHECK(alarms >= 20);
    CHECK(parts >= 2);  // 16 per part
    forge_ack_request ack{};
    set(ack.id, "all");
    forge_ack_reply acked{};
    REQUIRE(c.call(forge_msg_id_ack, ack, acked).ok());
    CHECK(acked.count >= 20);

    REQUIRE(forge::testing::wait_until([&] {
        std::lock_guard lock(m);
        bool state = false, audit = false;
        for (const auto& e : events) {
            if (e.topic == "state" && pb::view(e.as<forge_state_report>().id) == "freq") state = true;
            if (e.topic == "audit") {
                const auto a = e.as<forge_audit_record>();
                if (pb::view(a.action) == "set" && pb::view(a.target) == "freq" && pb::view(a.channel) == "link") {
                    audit = true;
                }
            }
        }
        return state && audit;
    }));
    r.stop();
    CHECK(r->freq == 2400.0);
    CHECK(r->presses == 1);
}

TEST_CASE("zmq: the same commands answer JSON over HTTP") {
    forge::testing::AppRunner<ControlledApp> r(
        {"-D", "http.enabled=true", "-D", "http.host=127.0.0.1", "-D", "http.port=0"});
    forge::http::Client http("http://127.0.0.1:" + std::to_string(r->http()->port()));
    // Values are JSON text in the messages: "433.0" is the number 433.
    const auto value = [](const json& reply) { return json::parse(reply["value"].get<std::string>()); };
    CHECK(value(http.post_json("/forge/api/tmtc/set", {{"id", "freq"}, {"value", "433"}})) == 433);
    CHECK(value(http.post_json("/forge/api/tmtc/get", {{"id", "freq"}})) == 433);
    CHECK(http.post_json("/forge/api/tmtc/2", json::object())["name"] == "zmqapp");  // by number: info
    const auto list = http.get_json("/forge/api/tmtc");
    CHECK(std::any_of(list.begin(), list.end(), [](const json& c) { return c["name"] == "logs"; }));
    try {
        http.post_json("/forge/api/tmtc/get", {{"id", "nothing"}});
        FAIL("no error");
    } catch (const forge::Error& e) {
        CHECK(e.status() == 404);
    }
    try {
        http.post_json("/forge/api/tmtc/get", {{"idd", "freq"}});
        FAIL("no error");
    } catch (const forge::Error& e) {
        CHECK(e.status() == 400);
        CHECK(std::string(e.what()).find("idd: not a field of forge.get_request") != std::string::npos);
    }
}
