// The whole application over its HTTP API, on the simulator.
#include "app.hpp"

#include <forge/testing.hpp>

#include <httplib.h>

using namespace la;
using namespace std::chrono_literals;
using forge::json;

namespace {

using Runner = forge::testing::AppRunner<LogicAnalyzer>;

std::vector<std::string> args(const forge::testing::TempDir& dir, const std::string& scenario = "mixed") {
    return {"-D", "http.enabled=true",
            "-D", "http.port=0",
            "-D", "http.host=\"127.0.0.1\"",
            "-D", "analyzer.scenario=\"" + scenario + "\"",
            "-D", "analyzer.captures_dir=\"" + (dir / "captures").string() + "\"",
            "-D", "analyzer.analyzers_file=\"" + (dir / "analyzers.json").string() + "\""};
}

/// Sets the controls, presses Start and waits for the capture to finish.
json capture(Runner& r, httplib::Client& http, double seconds, const json& controls = json::object()) {
    r->invoke([&] {
        r->dashboard()->set("duration_s", seconds);
        for (const auto& [k, v] : controls.items()) r->dashboard()->set(k, v);
        r->dashboard()->press("start");
    });
    json st;
    REQUIRE(forge::testing::wait_until([&] {
        st = json::parse(http.Get("/api/status")->body);
        return st["state"] == "idle" && !st["capture"].is_null() && st["capture"]["done"] == true &&
               !st["last_file"].get<std::string>().empty();
    }, 20s));
    return st;
}

json get(httplib::Client& http, const std::string& path) {
    auto res = http.Get(path);
    REQUIRE(res);
    INFO(path << ": " << res->body);
    REQUIRE(res->status == 200);
    return json::parse(res->body);
}

}  // namespace

TEST_CASE("app: capture, view, decode, table, measure") {
    forge::testing::TempDir dir;
    Runner r(args(dir));
    httplib::Client http("127.0.0.1", r->http()->port());

    // The simulator's analyzers are there from the start.
    auto analyzers = get(http, "/api/analyzers");
    REQUIRE(analyzers.size() == 2);
    CHECK(analyzers[0]["type"] == "uart");
    CHECK(analyzers[1]["type"] == "i2c");

    const json st = capture(r, http, 0.05);
    const auto& cap = st["capture"];
    CHECK(cap["duration_s"].get<double>() == doctest::Approx(0.05));
    CHECK(cap["edges"].get<int>() > 1000);
    CHECK(st["names"][0] == "UART TX");

    const auto end = cap["end"].get<std::uint64_t>();
    auto view = get(http, "/api/view?from=0&to=" + std::to_string(end) + "&px=800");
    REQUIRE(view["channels"].size() == 4);
    for (const auto& ch : view["channels"]) CHECK(ch["items"].size() <= 800);
    CHECK(view["channels"][3]["items"].size() > 10);  // the PWM

    auto frames = get(http, "/api/frames?from=0&to=200000&px=1000");  // the first 2 ms
    REQUIRE(frames["analyzers"].size() == 2);
    const auto& uart = frames["analyzers"][0];
    REQUIRE(!uart["frames"].empty());
    CHECK(uart["frames"][0][4] == "'H'");  // short text in ASCII
    // the whole capture at 100 px: too many to draw one by one
    frames = get(http, "/api/frames?from=0&to=" + std::to_string(end) + "&px=100");
    CHECK(!frames["analyzers"][0]["blocks"].empty());
    CHECK(frames["analyzers"][0]["frames"].empty());

    auto table = get(http, "/api/table?limit=50");
    CHECK(table["total"].get<int>() > 100);
    CHECK(table["rows"].size() == 50);
    table = get(http, "/api/table?q=read%20from%200x48");
    REQUIRE(table["total"].get<int>() >= 3);
    CHECK(table["rows"][0]["name"] == "I2C");
    table = get(http, "/api/table?analyzer=a1&limit=5");
    for (const auto& row : table["rows"]) CHECK(row["analyzer"] == "a1");

    auto m = get(http, "/api/measure?from=0&to=" + std::to_string(end));
    REQUIRE(m["channels"][3]["frequency_hz"].is_number());
    CHECK(m["channels"][3]["frequency_hz"].get<double>() == doctest::Approx(1000).epsilon(0.002));

    auto term = get(http, "/api/terminal");
    REQUIRE(term.size() == 1);
    CHECK(term[0]["text"].get<std::string>().starts_with("Hello from Arty Z7! #0\n"));
}

TEST_CASE("app: analyzers can be added, changed and removed; bad settings are refused") {
    forge::testing::TempDir dir;
    Runner r(args(dir, "spi"));
    httplib::Client http("127.0.0.1", r->http()->port());
    auto res =
        http.Post("/api/analyzers", json{{"type", "uart"}, {"settings", {{"channel", 2}, {"baud", 9600}}}}.dump(),
                  "application/json");
    REQUIRE(res->status == 200);
    const auto a = json::parse(res->body);
    CHECK(a["name"] == "Async serial (UART)");
    CHECK(a["settings"]["bits"] == 8);
    const std::string id = a["id"];

    res = http.Put("/api/analyzers/" + id,
                   json{{"name", "Debug port"}, {"radix", "ascii"}, {"settings", {{"baud", 115200}}}}.dump(),
                   "application/json");
    REQUIRE(res->status == 200);
    CHECK(json::parse(res->body)["settings"]["channel"] == 2);  // kept
    CHECK(json::parse(res->body)["settings"]["baud"] == 115200);

    CHECK(http.Post("/api/analyzers", R"({"type": "spi", "settings": {"sck": 0, "cs": 0}})", "application/json")
              ->status == 400);
    CHECK(http.Post("/api/analyzers", R"({"type": "can"})", "application/json")->status == 400);
    CHECK(http.Put("/api/analyzers/" + id, R"({"radix": "octal"})", "application/json")->status == 400);
    CHECK(http.Put("/api/analyzers/nope", "{}", "application/json")->status == 404);

    // Kept in analyzers_file: a restart finds them.
    REQUIRE(http.Delete("/api/analyzers/a1")->status == 204);
    r.stop();
    Runner again(args(dir, "spi"));
    httplib::Client http2("127.0.0.1", again->http()->port());
    const auto list = get(http2, "/api/analyzers");
    REQUIRE(list.size() == 1);
    CHECK(list[0]["name"] == "Debug port");
}

TEST_CASE("app: triggers, exports, saved captures, loading and importing") {
    forge::testing::TempDir dir;
    Runner r(args(dir, "spi"));
    httplib::Client http("127.0.0.1", r->http()->port());
    const json st =
        capture(r, http, 0.004, {{"trigger", "falling"}, {"trigger_channel", "0"}, {"pre_trigger_ms", 0.05}});
    REQUIRE(!st["capture"]["trigger"].is_null());
    const std::string file = st["last_file"];

    auto res = http.Get("/api/export/capture.vcd");
    REQUIRE(res->status == 200);
    CHECK(res->body.find("$var wire 1 ! CS $end") != std::string::npos);
    CHECK(res->get_header_value("Content-Disposition").find(".vcd") != std::string::npos);
    res = http.Get("/api/export/capture.csv");
    CHECK(res->body.starts_with("time_s,CS,SCK,MOSI,MISO\n"));
    res = http.Get("/api/export/decoded.csv");
    CHECK(res->body.starts_with("analyzer,type,kind,start_s,duration_s,value,text,error\n"));
    CHECK(res->body.find("\"MOSI 0x9F  MISO 0xFF\"") != std::string::npos);
    res = http.Get("/api/export/decoded.txt");
    CHECK(res->body.find("SPI flash     MOSI 0x9F") != std::string::npos);
    const auto lacap = http.Get("/api/export/capture.lacap")->body;
    CHECK(lacap.starts_with("LACAP1\n"));

    // In the store, downloadable, and it opens again.
    auto list = get(http, "/api/captures");
    REQUIRE(list.size() == 1);
    CHECK(list[0]["file"] == file);
    CHECK(list[0]["trigger"] == true);
    CHECK(http.Get("/api/captures/" + file)->body.starts_with("LACAP1\n"));
    CHECK(http.Get("/api/captures/..%2Fx.lacap")->status >= 400);
    CHECK(http.Get("/api/captures/x.txt")->status == 400);

    capture(r, http, 0.001, {{"trigger", "none"}});  // another one, then go back to the first
    REQUIRE(http.Post("/api/captures/load", json{{"file", file}}.dump(), "application/json")->status == 200);
    auto status = get(http, "/api/status");
    CHECK(status["loaded_file"] == file);
    CHECK(status["capture"]["trigger"] == st["capture"]["trigger"]);
    CHECK(get(http, "/api/table")["total"].get<int>() > 0);
    CHECK(http.Post("/api/captures/load", R"({"file": "nope.lacap"})", "application/json")->status == 404);

    // Upload a capture file (e.g. from another board).
    res = http.Post("/api/captures/import", lacap, "application/octet-stream");
    REQUIRE(res->status == 200);
    CHECK(get(http, "/api/captures").size() == 3);
    CHECK(http.Post("/api/captures/import", "not a capture", "application/octet-stream")->status == 422);

    // Back to live; Start clears the loaded file too.
    REQUIRE(http.Post("/api/captures/live", "{}", "application/json")->status == 204);
    CHECK(get(http, "/api/status")["loaded_file"] == "");
}

TEST_CASE("app: controls are checked") {
    forge::testing::TempDir dir;
    Runner r(args(dir));
    CHECK_THROWS(r->invoke([&] { r->dashboard()->set("pattern", "12XX"); }));
    CHECK_THROWS(r->invoke([&] { r->dashboard()->set("sample_rate", "3 MHz"); }));
    r->invoke([&] { r->dashboard()->set("sample_rate", "25 MHz"); });
    CHECK(r->settings().sample_hz == 25e6);
    r->invoke([&] { r->dashboard()->set("logic_level", "1.8 V"); });
    CHECK(r->settings().threshold_v == doctest::Approx(0.9));
    r->invoke([&] { r->dashboard()->set("threshold_v", 1.0); });
    CHECK(r->invoke([&] { return r->dashboard()->get("logic_level"); }) == "Custom");
    r->invoke([&] { r->dashboard()->set("trigger", "pattern"); });
    CHECK_THROWS(r->invoke([&] { r->dashboard()->press("start"); }));  // XXXX never fires
}
