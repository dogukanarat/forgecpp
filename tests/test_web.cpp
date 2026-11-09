#include <forge/forge.hpp>
#include <forge/testing.hpp>

#include <algorithm>
#include <atomic>
#include <future>
#include <thread>

using forge::json;
using forge::http::HttpError;

namespace {

int status_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const forge::Error& e) {
        return e.status();
    }
    return 200;
}

}  // namespace

TEST_CASE("dashboard: bound controls, validation and changes") {
    forge::web::Dashboard ui("Test");
    bool enabled = false;
    int speed = 10;
    double ratio = 0.5;
    std::string mode = "auto";
    std::string label = "x";
    int pressed = 0;
    double computed = 1.5;

    auto& g = ui.group("Motor");
    g.toggle("enabled", "Enabled", enabled);
    g.slider("speed", "Speed", speed, 0, 100, 5).unit("rpm");
    g.number("ratio", "Ratio", ratio).range(0, 1, 0.1).precision(2);
    g.select("mode", "Mode", {"auto", "manual"}, mode);
    g.text("label", "Label", label);
    g.button("go", "Go", [&] { ++pressed; }).confirm("sure?").danger();
    g.value("computed", "Computed", [&] { return computed * 2; });
    g.value("speed_copy", "Speed copy", speed);
    ui.link("Docs", "/docs.html");

    CHECK(&ui.group("Motor") == &g);  // same group returned
    CHECK_THROWS_AS(g.toggle("enabled", "dup", enabled), std::invalid_argument);
    CHECK_THROWS_AS(g.toggle("bad id", "x", enabled), std::invalid_argument);

    const auto state = ui.state();
    CHECK(state["enabled"] == false);
    CHECK(state["speed"] == 10);
    CHECK(state["computed"] == 3.0);
    CHECK_FALSE(state.contains("go"));

    ui.set("enabled", true);
    ui.set("speed", 42.4);  // rounded for integral variables
    ui.set("ratio", 0.25);
    ui.set("mode", "manual");
    ui.set("label", "hello");
    ui.set("go", nullptr);  // buttons are pressed through set() too
    ui.press("go");
    CHECK(enabled);
    CHECK(speed == 42);
    CHECK(ratio == doctest::Approx(0.25));
    CHECK(mode == "manual");
    CHECK(label == "hello");
    CHECK(pressed == 2);

    CHECK(status_of([&] { ui.set("missing", 1); }) == 404);
    CHECK(status_of([&] { ui.set("enabled", 1); }) == 400);
    CHECK(status_of([&] { ui.set("speed", 101); }) == 400);
    CHECK(status_of([&] { ui.set("speed", "fast"); }) == 400);
    CHECK(status_of([&] { ui.set("ratio", 2.0); }) == 400);
    CHECK(status_of([&] { ui.set("mode", "turbo"); }) == 400);
    CHECK(status_of([&] { ui.set("label", 5); }) == 400);
    CHECK(status_of([&] { ui.set("computed", 1); }) == 403);
    CHECK(status_of([&] { ui.press("enabled"); }) == 400);
    CHECK(speed == 42);  // rejected values never reach the variable

    // changes(): everything first, then only what changed
    CHECK(ui.changes().size() == state.size());
    CHECK(ui.changes().empty());
    computed = 10;
    speed = 7;
    const auto diff = ui.changes();
    CHECK(diff.size() == 3);  // computed, speed, speed_copy
    CHECK(diff["speed_copy"] == 7);

    const auto schema = ui.schema();
    CHECK(schema["title"] == "Test");
    CHECK(schema["links"][0]["href"] == "/docs.html");
    const auto& controls = schema["groups"][0]["controls"];
    CHECK(controls.size() == 8);
    CHECK(controls[1]["kind"] == "slider");
    CHECK(controls[1]["unit"] == "rpm");
    CHECK(controls[1]["max"] == 100.0);
    CHECK(controls[2]["precision"] == 2);
    CHECK(controls[5]["kind"] == "button");
    CHECK(controls[5]["confirm"] == "sure?");
    CHECK(controls[5]["style"] == "danger");
    CHECK(controls[6]["readonly"] == true);
}

TEST_CASE("dashboard: values from atomics and metrics") {
    forge::web::Dashboard ui("Test");
    std::atomic<std::uint64_t> frames{3};
    std::atomic<bool> busy{true};
    forge::metrics::Counter errors;
    forge::metrics::Gauge level;
    errors.inc(2);
    level.set(-3.5);
    auto& g = ui.group("Counters");
    g.value("frames", "Frames", frames);
    g.value("busy", "Busy", busy);
    g.value("errors", "Errors", errors);
    g.value("level", "Level", level).unit("dB");
    frames = 4;
    CHECK(ui.get("frames") == 4);
    CHECK(ui.get("busy") == true);
    CHECK(ui.get("errors") == 2);
    CHECK(ui.get("level") == -3.5);
}

TEST_CASE("web: mime types and built-in assets") {
    CHECK(forge::web::mime_type("/a/index.html") == "text/html; charset=utf-8");
    CHECK(forge::web::mime_type("x.JS") == "text/javascript; charset=utf-8");
    CHECK(forge::web::mime_type("logo.svg") == "image/svg+xml");
    CHECK(forge::web::mime_type("blob") == "application/octet-stream");

    std::vector<std::string> paths;
    for (const auto& a : forge::web::builtin_assets()) paths.emplace_back(a.path);
    CHECK(std::find(paths.begin(), paths.end(), "/forge/index.html") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "/forge/forge.js") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "/forge/forge.css") != paths.end());
}

TEST_CASE("http: in-memory assets, auth and event stream") {
    static const unsigned char index_html[] = "<h1>hi</h1>";
    static const unsigned char page_js[] = "console.log(1)";
    const forge::web::Asset assets[] = {{"/index.html", index_html, sizeof(index_html) - 1},
                                        {"/docs/index.html", index_html, sizeof(index_html) - 1},
                                        {"/js/page.js", page_js, sizeof(page_js) - 1}};

    forge::http::Server server;
    server.assets({std::begin(assets), std::end(assets)});
    server.get_json("/health", [](const auto&) { return json{{"ok", true}}; });
    server.get_json("/api/secret", [](const auto&) { return json{{"secret", 42}}; });

    forge::http::Auth auth;
    auth.user = "admin";
    auth.password = "pw";
    auth.token = "tok";
    server.auth(auth);

    auto stream = std::make_shared<forge::http::EventStream>(2);
    stream->on_connect([] { return std::vector<forge::http::EventStream::Message>{{"hello", {{"n", 1}}}}; });
    server.events("/events", stream);
    CHECK(server.has_asset("/js/page.js"));

    const int port = server.start("127.0.0.1", 0);
    httplib::Client raw("127.0.0.1", port);

    SUBCASE("authentication") {
        auto r = raw.Get("/api/secret");
        REQUIRE(r);
        CHECK(r->status == 401);
        CHECK(r->get_header_value("WWW-Authenticate").find("Basic") == 0);
        CHECK(raw.Get("/index.html")->status == 401);  // assets are protected too
        CHECK(raw.Get("/health")->status == 200);     // public path

        forge::http::Client basic("http://127.0.0.1:" + std::to_string(port));
        basic.basic_auth("admin", "pw");
        CHECK(basic.get_json("/api/secret")["secret"] == 42);

        forge::http::Client bearer("http://127.0.0.1:" + std::to_string(port));
        bearer.bearer_token("tok");
        CHECK(bearer.get_json("/api/secret")["secret"] == 42);

        forge::http::Client wrong("http://127.0.0.1:" + std::to_string(port));
        wrong.basic_auth("admin", "nope");
        CHECK_THROWS_AS(wrong.get_json("/api/secret"), HttpError);
    }

    SUBCASE("assets, directory index, redirect and ETag") {
        raw.set_bearer_token_auth("tok");
        auto r = raw.Get("/");
        REQUIRE(r);
        CHECK(r->status == 200);
        CHECK(r->body == "<h1>hi</h1>");
        CHECK(r->get_header_value("Content-Type") == "text/html; charset=utf-8");

        auto js = raw.Get("/js/page.js");
        CHECK(js->body == "console.log(1)");
        CHECK(js->get_header_value("Content-Type") == "text/javascript; charset=utf-8");

        auto redirect = raw.Get("/docs");
        CHECK(redirect->status == 302);
        CHECK(redirect->get_header_value("Location") == "/docs/");
        CHECK(raw.Get("/docs/")->body == "<h1>hi</h1>");

        const auto etag = js->get_header_value("ETag");
        CHECK_FALSE(etag.empty());
        auto cached = raw.Get("/js/page.js", httplib::Headers{{"If-None-Match", etag}});
        CHECK(cached->status == 304);
        CHECK(raw.Get("/nope.js")->status == 404);
    }

    SUBCASE("server-sent events") {
        std::string received;
        std::atomic<bool> done{false};
        std::thread reader([&] {
            httplib::Client c("127.0.0.1", port);
            c.set_bearer_token_auth("tok");
            c.set_read_timeout(std::chrono::seconds(5));
            c.Get("/events", [&](const char* data, std::size_t len) {
                received.append(data, len);
                return received.find("event: tick") == std::string::npos;  // stop after the tick
            });
            done = true;
        });
        REQUIRE(forge::testing::wait_until([&] { return stream->clients() == 1; }));
        stream->publish("tick", {{"value", 7}});
        reader.join();
        CHECK(received.find("event: hello\ndata: {\"n\":1}\n\n") != std::string::npos);
        CHECK(received.find("event: tick\ndata: {\"value\":7}\n\n") != std::string::npos);
        CHECK(forge::testing::wait_until([&] { return stream->clients() == 0; }));
    }

    SUBCASE("event stream client limit and shutdown") {
        std::vector<std::thread> readers;
        for (int i = 0; i < 2; ++i) {
            readers.emplace_back([port] {
                httplib::Client c("127.0.0.1", port);
                c.set_bearer_token_auth("tok");
                c.Get("/events", [](const char*, std::size_t) { return true; });
            });
        }
        REQUIRE(forge::testing::wait_until([&] { return stream->clients() == 2; }));
        raw.set_bearer_token_auth("tok");
        CHECK(raw.Get("/events")->status == 503);  // max_clients = 2

        // stop() must release the long-lived event connections promptly
        const auto t0 = std::chrono::steady_clock::now();
        server.stop();
        for (auto& t : readers) t.join();
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
        CHECK(stream->closed());
    }
}

TEST_CASE("application: web dashboard API end to end") {
    struct App : forge::Application {
        App() : Application({"webapp", "2.0.0"}) {}
        int level = 3;
        bool on = false;
        int presses = 0;

    protected:
        void setup_web(forge::web::Dashboard& ui) override {
            auto& g = ui.group("Main");
            g.slider("level", "Level", level, 0, 10);
            g.toggle("on", "On", on);
            g.button("kick", "Kick", [this] { ++presses; });
        }
    };

    std::vector<std::string> args{"webapp", "--http",           "127.0.0.1:0", "-D", "log.console=false",
                                  "-D",     "web.refresh_ms=50"};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());

    App app;
    std::promise<int> port;
    app.hook_start([&](forge::Application& a) { a.post([&] { port.set_value(a.http()->port()); }); });
    std::thread runner([&] { CHECK(app.run(static_cast<int>(argv.size()), argv.data()) == 0); });
    const int p = port.get_future().get();

    forge::http::Client client("http://127.0.0.1:" + std::to_string(p));
    CHECK(client.get_json("/forge/api/info")["name"] == "webapp");
    CHECK(client.get_json("/forge/api/ui")["groups"][0]["controls"].size() == 3);
    CHECK(client.get_json("/forge/api/state")["level"] == 3);

    CHECK(client.post_json("/forge/api/controls/level", {{"value", 8}})["value"] == 8);
    CHECK(client.post_json("/forge/api/controls/on", {{"value", true}})["value"] == true);
    client.post_json("/forge/api/controls/kick", json::object());
    CHECK(client.get_json("/forge/api/controls/level")["value"] == 8);
    CHECK_THROWS_AS(client.post_json("/forge/api/controls/level", {{"value", 11}}), HttpError);
    CHECK_THROWS_AS(client.post_json("/forge/api/controls/level", json::object()), HttpError);

    // A change made on the main loop reaches the browser through the event stream.
    std::string received;
    std::thread reader([&] {
        httplib::Client c("127.0.0.1", p);
        c.Get("/forge/api/events", [&](const char* data, std::size_t len) {
            received.append(data, len);
            return received.find("\"level\":1") == std::string::npos;
        });
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    app.post([&] { app.level = 1; });
    reader.join();
    CHECK(received.find("event: state") != std::string::npos);

    // Dashboard and built-in pages are served.
    httplib::Client raw("127.0.0.1", p);
    CHECK(raw.Get("/")->status == 302);
    CHECK(raw.Get("/forge/")->status == 200);
    const auto logs = client.get_json("/forge/api/logs");
    CHECK(logs.is_array());
    CHECK_FALSE(logs.empty());

    app.stop();
    runner.join();
    CHECK(app.level == 1);
    CHECK(app.on);
    CHECK(app.presses == 1);
}
