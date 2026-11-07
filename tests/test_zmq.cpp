#include <forge/forge.hpp>
#include <forge/testing.hpp>
#include <forge/zmq.hpp>

#include <atomic>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using forge::json;
using namespace std::chrono_literals;
namespace fz = forge::zmq;

TEST_CASE("zmq: pub/sub with header, payload and topic filtering") {
    fz::Sender pub(fz::Sender::Kind::pub, "tcp://127.0.0.1:*");
    const auto endpoint = pub.endpoint();
    REQUIRE(endpoint.rfind("tcp://127.0.0.1:", 0) == 0);

    std::mutex m;
    std::vector<fz::Message> got;
    fz::Receiver::Options opts;
    opts.topics = {"iq"};
    fz::Receiver sub(fz::Receiver::Kind::sub, endpoint, [&](fz::Message&& msg) {
        std::lock_guard lock(m);
        got.push_back(std::move(msg));
    }, opts);

    // Subscriptions propagate asynchronously ("slow joiner"): wait for them.
    REQUIRE(forge::testing::wait_until([&] { return pub.subscriptions() == 1; }));

    const std::vector<std::int16_t> samples{1, -1, 32767, -32768};
    CHECK(pub.send("status", {{"ignored", true}}));
    CHECK(pub.send("iq", {{"seq", 7}}, samples.data(), samples.size() * sizeof(std::int16_t)));
    REQUIRE(forge::testing::wait_until([&] { std::lock_guard lock(m); return got.size() == 1; }));

    std::lock_guard lock(m);
    CHECK(got[0].topic == "iq");
    CHECK(got[0].header["seq"] == 7);
    REQUIRE(got[0].payload.size() == 8);
    std::int16_t back[4];
    std::memcpy(back, got[0].payload.data(), sizeof(back));
    CHECK(back[2] == 32767);
    CHECK(back[3] == -32768);
    CHECK(pub.sent() == 2);
}

TEST_CASE("zmq: subscriptions drop when subscribers leave") {
    fz::Sender pub(fz::Sender::Kind::pub, "tcp://127.0.0.1:*");
    {
        fz::Receiver a(fz::Receiver::Kind::sub, pub.endpoint(), [](fz::Message&&) {});
        fz::Receiver b(fz::Receiver::Kind::sub, pub.endpoint(), [](fz::Message&&) {});
        REQUIRE(forge::testing::wait_until([&] { return pub.subscriptions() == 2; }));
    }
    CHECK(forge::testing::wait_until([&] { return pub.subscriptions() == 0; }));
}

TEST_CASE("zmq: push/pull is lossless and reports drops without a peer") {
    fz::Sender push(fz::Sender::Kind::push, "tcp://127.0.0.1:*");
    CHECK_FALSE(push.send("job", {{"n", 0}}));  // nobody connected yet
    CHECK(push.dropped() == 1);

    std::atomic<int> sum{0}, count{0};
    fz::Receiver pull(fz::Receiver::Kind::pull, push.endpoint(), [&](fz::Message&& m) {
        sum += m.header["n"].get<int>();
        ++count;
    });
    REQUIRE(forge::testing::wait_until([&] { return push.send("job", {{"n", 1}}); }));
    for (int i = 2; i <= 100; ++i) {
        while (!push.send("job", {{"n", i}})) std::this_thread::sleep_for(1ms);
    }
    REQUIRE(forge::testing::wait_until([&] { return count == 100; }));
    CHECK(sum == 5050);
}

TEST_CASE("zmq: request/reply server, errors and client timeout") {
    fz::Server server("tcp://127.0.0.1:*", [](const json& req) -> json {
        const auto cmd = req.value("cmd", "");
        if (cmd == "add") return {{"sum", req.at("a").get<int>() + req.at("b").get<int>()}};
        if (cmd == "forbidden") throw forge::http::HttpError(403, "nope");
        if (cmd == "crash") throw std::runtime_error("boom");
        throw fz::Error("unknown command", 400);
    });

    fz::Client client(server.endpoint(), 2s);
    CHECK(client.call("add", {{"a", 2}, {"b", 3}})["sum"] == 5);
    auto raw = client.request({{"cmd", "crash"}});
    CHECK(raw["ok"] == false);
    CHECK(raw["status"] == 500);
    try {
        client.call("forbidden");
        FAIL("expected Error");
    } catch (const fz::Error& e) {
        CHECK(e.status() == 403);
    }
    CHECK_THROWS_AS(client.call("add", {{"a", 1}}), fz::Error);  // missing "b" -> 400
    CHECK(server.requests() == 4);

    // Timeout against a server that is gone; the client recovers afterwards.
    const auto endpoint = server.endpoint();
    server.stop();
    client.timeout(200ms);
    CHECK_THROWS_AS(client.call("add", {{"a", 1}, {"b", 1}}), fz::Error);
    fz::Server again(endpoint, [](const json&) { return json{{"back", true}}; });
    client.timeout(2s);
    CHECK(client.call("anything")["back"] == true);
}

TEST_CASE("zmq: serve_application exposes controls and alarms") {
    struct App : forge::Application {
        App() : Application({"zmqapp", "3.0.0"}) {}
        double freq = 100.0;
        int presses = 0;

    protected:
        void setup_web(forge::web::Dashboard& ui) override {
            auto& g = ui.group("Radio");
            g.number("freq", "Frequency", freq).range(70, 6000);
            g.button("cal", "Calibrate", [this] { ++presses; });
        }
        void on_start() override { server_ = fz::serve_application(*this, "tcp://127.0.0.1:*"); }
        void on_stop() override { server_.reset(); }

    public:
        std::unique_ptr<fz::Server> server_;
    };

    // No HTTP at all: controls must still work over ZeroMQ.
    std::vector<std::string> args{"zmqapp", "-D", "log.console=false"};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());

    App app;
    std::promise<std::string> endpoint;
    app.hook_start([&](forge::Application&) { app.post([&] { endpoint.set_value(app.server_->endpoint()); }); });
    std::thread runner([&] { app.run(static_cast<int>(argv.size()), argv.data()); });

    fz::Client c(endpoint.get_future().get());
    CHECK(c.call("ping")["pong"] == true);
    CHECK(c.call("info")["version"] == "3.0.0");
    CHECK(c.call("ui")["ui"]["groups"][0]["controls"].size() == 2);
    CHECK(c.call("set", {{"id", "freq"}, {"value", 2400}})["value"] == 2400.0);
    CHECK(c.call("get", {{"id", "freq"}})["value"] == 2400.0);
    CHECK(c.call("state")["state"]["freq"] == 2400.0);
    c.call("press", {{"id", "cal"}});

    auto expect_status = [&](const json& req, int status) {
        const auto reply = c.request(req);
        CHECK(reply["ok"] == false);
        CHECK(reply["status"] == status);
    };
    expect_status({{"cmd", "set"}, {"id", "freq"}, {"value", 9999}}, 400);  // out of range
    expect_status({{"cmd", "set"}, {"id", "nope"}, {"value", 1}}, 404);
    expect_status({{"cmd", "get"}}, 400);
    expect_status({{"cmd", "bogus"}}, 400);

    app.alarms().raise("rf.overflow", forge::Severity::warning, "RX overflow");
    CHECK(c.call("alarms")["alarms"].size() == 1);
    CHECK(c.call("ack", {{"id", "all"}})["acknowledged"] == 1);
    CHECK(c.call("info")["alarms"]["pending"] == 0);

    app.stop();
    runner.join();
    CHECK(app.freq == 2400.0);
    CHECK(app.presses == 1);
}
