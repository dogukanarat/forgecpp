// forge.v1 over ZeroMQ: framing, streams, request/reply, the control service
// and the events of an application.
#include <forge/control.hpp>
#include <forge/forge.hpp>
#include <forge/proto.hpp>
#include <forge/testing.hpp>
#include <forge/trace.hpp>
#include <forge/zmq.hpp>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using forge::json;
using namespace std::chrono_literals;
namespace fz = forge::zmq;
namespace v1 = forge::v1;

namespace {
v1::LogRecord record(const std::string& text) {
    v1::LogRecord r;
    r.set_message(text);
    r.set_level("info");
    return r;
}
}  // namespace

TEST_CASE("zmq: forge.v1 framing - header, typed body, blob and topic filtering") {
    forge::proto::set_source("zmq-test");
    fz::Sender pub(fz::Sender::Kind::pub, "tcp://127.0.0.1:*");
    REQUIRE(pub.endpoint().rfind("tcp://127.0.0.1:", 0) == 0);

    std::mutex m;
    std::vector<fz::Message> got;
    fz::Receiver::Options opts;
    opts.topics = {"iq"};
    fz::Receiver sub(fz::Receiver::Kind::sub, pub.endpoint(), [&](fz::Message&& msg) {
        std::lock_guard lock(m);
        got.push_back(std::move(msg));
    }, opts);
    // Subscriptions propagate asynchronously ("slow joiner"): wait for them.
    REQUIRE(forge::testing::wait_until([&] { return pub.subscriptions() == 1; }));

    const std::vector<std::int16_t> samples{1, -1, 32767, -32768};
    CHECK(pub.publish("status", record("ignored")));
    CHECK(pub.publish("iq", record("first"), samples.data(), samples.size() * sizeof(std::int16_t)));
    CHECK(pub.publish("iq", record("second")));
    REQUIRE(forge::testing::wait_until([&] { std::lock_guard lock(m); return got.size() == 2; }));

    std::lock_guard lock(m);
    const auto& a = got[0];
    CHECK(a.topic == "iq");
    CHECK(a.header.source() == "zmq-test");
    CHECK(a.header.type() == "forge.v1.LogRecord");
    CHECK(a.header.seq() == 1);
    CHECK(got[1].header.seq() == 2);
    CHECK(a.header.time_unix_ns() > 0);
    CHECK(a.as<v1::LogRecord>().message() == "first");
    CHECK_THROWS_AS(a.as<v1::AlarmList>(), fz::Error);  // wrong type
    REQUIRE(a.blob.size() == 8);
    std::int16_t back[4];
    std::memcpy(back, a.blob.data(), sizeof(back));
    CHECK(back[2] == 32767);
    CHECK(back[3] == -32768);
    CHECK(got[1].blob.empty());
}

TEST_CASE("zmq: publisher streams - subscribers per topic, rate limit, latest-only, zero copy, loss count") {
    fz::Publisher pub("tcp://127.0.0.1:*");
    auto& fast = pub.add("fast", "forge.v1.LogRecord");
    auto& slow = pub.add("slow", "forge.v1.LogRecord", {"latest", 5});
    CHECK_FALSE(fast.wanted());  // nobody listens yet

    std::mutex m;
    std::vector<fz::Message> got;
    fz::Receiver::Options opts;
    opts.topics = {"fast"};
    fz::Receiver sub(fz::Receiver::Kind::sub, pub.endpoint(), [&](fz::Message&& msg) {
        std::lock_guard lock(m);
        got.push_back(std::move(msg));
    }, opts);
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
    REQUIRE(forge::testing::wait_until([&] { std::lock_guard lock(m); return got.size() == 10; }));
    CHECK(forge::testing::wait_until([&] { return weak.expired(); }));
    {
        std::lock_guard lock(m);
        CHECK(got[0].blob.size() == 2048);
        CHECK(got[9].header.seq() == 10);
    }
    CHECK(fast.info().sent() == 10);
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
    CHECK(slow.info().dropped() > 0);

    const auto list = pub.list();
    REQUIRE(list.size() == 2);
    CHECK(list[0].name() == "fast");
    CHECK(list[0].type() == "forge.v1.LogRecord");
}

TEST_CASE("zmq: request/reply server and a client that recovers from timeouts") {
    fz::Server server("tcp://127.0.0.1:*", [](std::string_view req) { return "echo:" + std::string(req); });
    fz::Client client(server.endpoint(), 2s);
    CHECK_THROWS_AS(client.request_json({{"a", 1}}), fz::Error);  // "echo:..." is not JSON
    const auto endpoint = server.endpoint();
    server.stop();
    client.timeout(200ms);
    CHECK_THROWS_AS(client.request_json({{"cmd", "ping"}}), fz::Error);
    fz::Server again(endpoint, [](std::string_view) { return std::string(R"({"back":true})"); });
    client.timeout(2s);
    CHECK(client.request_json({{"cmd", "x"}})["back"] == true);
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
}  // namespace

TEST_CASE("zmq: an application's control endpoint (forge.v1 and the older JSON) and its events") {
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
    c.set_client("test-suite");
    const auto info = c.info();
    CHECK(info.name() == "zmqapp");
    CHECK(info.version() == "3.0.0");
    CHECK(info.protocol() == "forge.v1");
    CHECK(info.endpoints().at("control") == ep.at("control"));

    CHECK(c.set("freq", 2400) == 2400);
    CHECK(c.get("freq") == 2400);
    c.press("cal");
    CHECK(c.call_command("scan", {{"to", 600}})["width"] == 500);
    try {
        c.set("freq", 9999);
        FAIL("expected an error");
    } catch (const fz::Error& e) {
        CHECK(e.status() == 400);
    }
    try {
        c.call_command("scan", {});  // "to" is required
        FAIL("expected an error");
    } catch (const fz::Error& e) {
        CHECK(e.status() == 400);
    }

    // The reply continues the request's trace.
    forge::v1::Request req;
    req.mutable_ping();
    const auto trace = forge::trace::new_trace();
    *req.mutable_trace() = forge::trace::to_proto(trace);
    const auto reply = c.request(req);
    CHECK(reply.ok());
    CHECK(forge::trace::from_proto(reply.trace()).trace_hex() == trace.trace_hex());

    // Older JSON commands on the same socket.
    CHECK(c.request_json({{"cmd", "get"}, {"id", "freq"}})["value"] == 2400);
    CHECK(c.request_json({{"cmd", "set"}, {"id", "freq"}, {"value", 9999}})["status"] == 400);
    CHECK(c.request_json({{"cmd", "bogus"}})["status"] == 400);

    r->alarms().raise("rf.overflow", forge::Severity::warning, "RX overflow");
    forge::v1::Request alarms;
    alarms.mutable_alarms();
    CHECK(c.call(alarms).alarms().alarms_size() >= 1);
    forge::v1::Request ack;
    ack.mutable_ack()->set_id("all");
    CHECK(c.call(ack).count() >= 1);

    REQUIRE(forge::testing::wait_until([&] {
        std::lock_guard lock(m);
        bool state = false, audit = false;
        for (const auto& e : events) {
            if (e.topic == "state" && e.as<v1::Values>().values().count("freq")) state = true;
            if (e.topic == "audit") {
                const auto a = e.as<v1::AuditRecord>();
                if (a.action() == "set" && a.target() == "freq" && a.client() == "test-suite") audit = true;
            }
        }
        return state && audit;
    }));
    r.stop();
    CHECK(r->freq == 2400.0);
    CHECK(r->presses == 1);
}
