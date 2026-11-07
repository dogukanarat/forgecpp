// Framework services: metrics, trace, config schema, roles, leases, audit,
// commands, persistence, health, buffer pool, real-time settings, telnet,
// HTTP RPC, JSON logs and crash reports.
#include <forge/buffer_pool.hpp>
#include <forge/control.hpp>
#include <forge/crash.hpp>
#include <forge/forge.hpp>
#include <forge/proto.hpp>
#include <forge/rt.hpp>
#include <forge/testing.hpp>
#include <forge/trace.hpp>

#include <httplib.h>

#include <arpa/inet.h>
#include <fstream>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

using forge::json;
using namespace std::chrono_literals;
namespace v1 = forge::v1;

TEST_CASE("metrics: counters, gauges, histograms, Prometheus text and snapshots") {
    forge::metrics::Registry reg;
    reg.set_labels({{"app", "t"}});
    auto& c = reg.counter("rx.frames", "Frames");
    c.inc();
    c.inc(4);
    auto& g = reg.gauge("rx.level", "Level", "dB");
    g.set(-3.5);
    g.add(1);
    auto& h = reg.histogram("rx.read_seconds", {0.001, 0.01, 0.1}, "Read time", "s");
    h.observe(0.0005);
    h.observe(0.05);
    h.observe(5);
    reg.gauge_fn("queue.depth", [] { return 7.0; });
    CHECK(&reg.counter("rx.frames") == &c);
    CHECK_THROWS_AS(reg.gauge("rx.frames"), std::invalid_argument);

    const auto text = reg.prometheus();
    CHECK(text.find("# TYPE rx_frames counter\nrx_frames{app=\"t\"} 5\n") != std::string::npos);
    CHECK(text.find("rx_level{app=\"t\"} -2.5") != std::string::npos);
    CHECK(text.find("rx_read_seconds_bucket{app=\"t\",le=\"0.01\"} 1") != std::string::npos);
    CHECK(text.find("rx_read_seconds_bucket{app=\"t\",le=\"+Inf\"} 3") != std::string::npos);
    CHECK(text.find("queue_depth{app=\"t\"} 7") != std::string::npos);

    const auto snap = reg.snapshot("rx.");
    CHECK(snap.metrics_size() == 3);
    const auto hist = std::find_if(snap.metrics().begin(), snap.metrics().end(),
                                   [](const v1::Metric& m) { return m.name() == "rx.read_seconds"; });
    REQUIRE(hist != snap.metrics().end());
    CHECK(hist->count() == 3);
    CHECK(hist->buckets(0) == 1);
    CHECK(hist->buckets(2) == 2);
}

TEST_CASE("trace: spans, counters, thread names and the trace id in a Chrome trace") {
    forge::trace::configure(true, 4096);
    forge::trace::set_thread_name("tester");
    const auto ctx = forge::trace::new_trace();
    {
        forge::trace::ContextScope scope(ctx);
        FORGE_TRACE_SCOPE("outer");
        {
            FORGE_TRACE_SCOPE("inner", "dsp");
            std::this_thread::sleep_for(1ms);
        }
        forge::trace::counter("depth", 3);
        forge::trace::instant("mark");
    }
    const auto d = forge::trace::dump();
    const auto j = json::parse(d.chrome_json);
    bool outer = false, inner = false, named = false, counter = false;
    const auto lo = ctx.trace_hex().substr(16);
    for (const auto& e : j["traceEvents"]) {
        if (e["ph"] == "X" && e["name"] == "outer") outer = e["args"]["trace"] == lo;
        if (e["ph"] == "X" && e["name"] == "inner") inner = e["cat"] == "dsp" && e["dur"].get<double>() >= 1000;
        if (e["ph"] == "M" && e["args"]["name"] == "tester") named = true;
        if (e["ph"] == "C" && e["name"] == "depth") counter = e["args"]["value"] == 3;
    }
    CHECK(outer);
    CHECK(inner);
    CHECK(named);
    CHECK(counter);
    forge::trace::configure(false, 4096);
    const auto before = forge::trace::dump().events;
    { FORGE_TRACE_SCOPE("ignored"); }
    CHECK(forge::trace::dump().events == before);
    forge::trace::configure(true, 65536);
}

TEST_CASE("config schema: types, ranges, choices, unknown keys and help") {
    forge::config::Schema s;
    s.section("radio", "Radio");
    s.number("radio.rate", 1e6, "Rate").range(1e5, 1e7).unit("Hz");
    s.string("radio.backend", "sim").choices({"sim", "iio"});
    s.array("radio.channels", json::array({1}));
    s.boolean("radio.enabled", true).required();
    s.open_section("radio.extra");

    auto check = [&](const std::string& toml) { return s.validate(forge::Config::parse(toml)); };
    CHECK(check("[radio]\nenabled = true\nrate = 2e6\nbackend = 'iio'\nchannels = [1, 2]\n[radio.extra]\nx = 1").ok());
    auto r = check("[radio]\nenabled = 1\nrate = 9e9\nbackend = 'usb'\nchanels = [1]");
    REQUIRE(r.errors.size() == 3);
    CHECK(r.errors[0].find("radio.backend") == 0);
    CHECK(r.errors[1].find("radio.enabled: expected bool") == 0);
    CHECK(r.errors[2].find("radio.rate") == 0);
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0] == "radio.chanels: unknown key (did you mean radio.channels?)");
    CHECK_FALSE(check("[radio]\nrate = 1e6").ok());  // enabled is required
    const auto help = s.help();
    CHECK(help.find("[radio]") != std::string::npos);
    CHECK(help.find("rate = 1000000.0") != std::string::npos);
    CHECK(help.find("sim|iio") != std::string::npos);
}

namespace {
struct RolesApp : forge::Application {
    RolesApp() : Application({"rolesapp", "1.0"}) {}
    double gain = 10;
    double power = 1;
    int calibrations = 0;

protected:
    void setup_config(forge::config::Schema& s) override { s.number("rolesapp.limit", 5, "A limit").range(0, 10); }
    void setup_web(forge::web::Dashboard& ui) override {
        auto& g = ui.group("RF");
        g.number("gain", "Gain", gain).range(0, 70).persist();
        g.number("power", "Power", power).range(0, 10).role(forge::Role::admin);
        g.button("cal", "Calibrate", [this] { ++calibrations; });
        g.command("sweep", "Sweep",
                  {forge::web::number_param("start", 100).range(70, 6000), forge::web::integer_param("points", 101),
                   forge::web::select_param("mode", {"fast", "slow"}, "fast"), forge::web::bool_param("save", false)},
                  [](const json& a) { return a; });
    }
};
}  // namespace

TEST_CASE("control: roles, leases, audit, commands and persistence") {
    forge::testing::TempDir dir;
    const auto state = (dir / "state.json").string();
    std::vector<std::string> args{"-D", "auth.enabled=true",
                                  "-D", "auth.users.vera.token=\"v\"",   "-D", "auth.users.vera.role=\"viewer\"",
                                  "-D", "auth.users.otto.token=\"o\"",   "-D", "auth.users.otto.role=\"operator\"",
                                  "-D", "auth.users.otto.password=\"pw\"",
                                  "-D", "auth.users.ada.token=\"a\"",    "-D", "auth.users.ada.role=\"admin\"",
                                  "-D", "state.file=\"" + state + "\""};
    {
        forge::testing::AppRunner<RolesApp> r(args);
        auto& svc = r->control();
        const auto vera = svc.identify("zmq", "v-host", "v");
        const auto otto = svc.identify("zmq", "o-host", "o");
        const auto ada = svc.identify("zmq", "a-host", "a");
        const auto anon = svc.identify("zmq", "x", "");
        CHECK(vera.role == forge::Role::viewer);
        CHECK(otto.user == "otto");
        CHECK(anon.role == forge::Role::viewer);  // default_role

        auto status = [](auto&& fn) {
            try {
                fn();
            } catch (const forge::Error& e) {
                return e.status();
            }
            return 200;
        };
        CHECK(svc.get("gain", vera) == 10);
        CHECK(status([&] { svc.set("gain", 20, vera); }) == 403);
        CHECK(status([&] { svc.set("gain", 20, anon); }) == 401);
        CHECK(svc.set("gain", 20, otto) == 20);
        CHECK(status([&] { svc.set("power", 2, otto); }) == 403);  // admin control
        CHECK(svc.set("power", 2, ada) == 2);
        CHECK(status([&] { svc.set_log_level("", "debug", otto); }) == 403);

        // Leases: otto holds the RF group; ada cannot change gain until released.
        CHECK(svc.lease("group:RF", 30, otto).count() == 30);
        CHECK(svc.lease_holder("gain") == otto.describe());
        CHECK(status([&] { svc.set("gain", 30, ada); }) == 409);
        CHECK(svc.set("gain", 25, otto) == 25);
        CHECK(status([&] { svc.release("group:RF", vera); }) == 403);
        svc.release("group:RF", otto);
        CHECK(svc.set("gain", 30, ada) == 30);

        // Commands: defaults, conversion, validation.
        const auto res = svc.call("sweep", {{"points", 11}, {"save", "yes"}}, otto);
        CHECK(res == json{{"start", 100}, {"points", 11}, {"mode", "fast"}, {"save", true}});
        CHECK(status([&] { svc.call("sweep", {{"start", 1}}, otto); }) == 400);
        CHECK(status([&] { svc.call("sweep", {{"mode", "turbo"}}, otto); }) == 400);
        CHECK(status([&] { svc.call("sweep", {{"bogus", 1}}, otto); }) == 400);
        CHECK(status([&] { svc.call("sweep", {}, vera); }) == 403);
        svc.press("cal", otto);

        // Audit: who, what, old and new value; refusals too.
        const auto audit = r->audit().list();
        auto find = [&](const std::string& user, const std::string& target, bool ok) {
            return std::find_if(audit.begin(), audit.end(), [&](const v1::AuditRecord& a) {
                return a.user() == user && a.target() == target && a.ok() == ok;
            });
        };
        auto set = find("otto", "gain", true);
        REQUIRE(set != audit.end());
        CHECK(forge::proto::to_json(set->old_value()) == 10);
        CHECK(forge::proto::to_json(set->new_value()) == 20);
        CHECK(set->channel() == "zmq");
        CHECK(set->client() == "o-host");
        CHECK(find("vera", "gain", false) != audit.end());
        CHECK(find("otto", "sweep", true)->action() == "call");

        // forge.v1 request with a token
        v1::Request req;
        req.set_token("v");
        req.mutable_set()->set_id("gain");
        *req.mutable_set()->mutable_value() = forge::proto::to_value(40);
        const auto reply = svc.handle(req, svc.identify("zmq", "x", req.token()));
        CHECK_FALSE(reply.ok());
        CHECK(reply.status() == 403);
        CHECK(reply.error().find("vera") != std::string::npos);
        // schema shows roles and persistence
        const auto schema = svc.schema();
        const auto power = std::find_if(schema.controls().begin(), schema.controls().end(),
                                        [](const v1::Control& c) { return c.id() == "power"; });
        CHECK(power->role() == "admin");
        CHECK(schema.commands(0).params_size() == 4);
    }
    // persist(): gain survives a restart, power does not.
    CHECK(forge::load_json(state)["gain"] == 30);
    forge::testing::AppRunner<RolesApp> again(args);
    CHECK(again->gain == 30);
    CHECK(again->power == 1);
}

TEST_CASE("config: an invalid configuration stops the start with the reason") {
    forge::testing::TempDir dir;
    const auto cfg = (dir / "bad.toml").string();
    std::ofstream(cfg) << "[rolesapp]\nlimit = 50\n[log]\nlevel = 'loud'\n";
    std::vector<std::string> args{"rolesapp", "-c", cfg, "-D", "log.console=false"};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    RolesApp app;
    CHECK(app.run(static_cast<int>(argv.size()), argv.data()) == 1);
}

TEST_CASE("health: components, overall state, strict /health and HTTP endpoints") {
    forge::testing::AppRunner<RolesApp> r({"-D", "http.enabled=true", "-D", "http.port=0", "-D", "http.host=\"127.0.0.1\""});
    httplib::Client http("127.0.0.1", r->http()->port());
    r->health().set("radio", forge::HealthState::ok);
    r->health().set("gps", forge::HealthState::failed, "no fix", false);
    CHECK(r->health().overall() == forge::HealthState::degraded);
    auto res = http.Get("/health?strict=1");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(json::parse(res->body)["health"] == "degraded");
    r->health().set("radio", forge::HealthState::failed, "device lost");
    res = http.Get("/health?strict=1");
    CHECK(res->status == 503);
    CHECK(http.Get("/health")->status == 200);
    CHECK(r->health().critical_failed_for().has_value());

    // Prometheus, metrics JSON, trace download.
    res = http.Get("/metrics");
    CHECK(res->body.find("process_rss_bytes{app=\"rolesapp\"}") != std::string::npos);
    CHECK(res->body.find("forge_loop_lag_seconds_bucket") != std::string::npos);
    res = http.Get("/forge/api/trace?seconds=60");
    CHECK(json::parse(res->body).contains("traceEvents"));

    // forge.v1 over HTTP: protobuf and its JSON mapping.
    v1::Request req;
    req.mutable_set()->set_id("gain");
    *req.mutable_set()->mutable_value() = forge::proto::to_value(33);
    std::string body;
    req.SerializeToString(&body);
    res = http.Post("/forge/api/rpc", body, "application/x-protobuf");
    v1::Reply reply;
    REQUIRE(reply.ParseFromString(res->body));
    CHECK(reply.ok());
    CHECK(forge::proto::to_json(reply.value()) == 33);
    res = http.Post("/forge/api/rpc", R"({"get":{"id":"gain"}})", "application/json");
    CHECK(json::parse(res->body)["value"] == 33);
    // The web JSON API goes through the same audit trail.
    res = http.Post("/forge/api/controls/gain", R"({"value": 34})", "application/json");
    CHECK(res->status == 200);
    const auto audit = r->audit().list(1);
    CHECK(audit.back().channel() == "web");
    CHECK(r->gain == 34);
}

TEST_CASE("buffer pool: buffers are recycled, allocations stay flat") {
    auto pool = forge::BufferPool<std::int16_t>::create(1024, 2, 4);
    CHECK(pool->allocations() == 2);
    {
        auto a = pool->acquire();
        auto b = pool->acquire();
        auto c = pool->acquire();  // pool empty: one new allocation
        CHECK(a->size() == 1024);
        CHECK(pool->allocations() == 3);
    }
    CHECK(pool->free_buffers() == 3);
    for (int i = 0; i < 100; ++i) {
        auto x = pool->acquire();
        (*x)[0] = static_cast<std::int16_t>(i);
    }
    CHECK(pool->allocations() == 3);
    std::shared_ptr<std::vector<std::int16_t>> outlives;
    {
        auto p2 = forge::BufferPool<std::int16_t>::create(16, 1);
        outlives = p2->acquire();
    }
    CHECK(outlives->size() == 16);  // the pool is gone; the buffer is still valid
}

TEST_CASE("rt: thread settings from the configuration") {
    const auto cfg = forge::Config::parse("[threads.rx]\npolicy = 'other'\nnice = 0\ncpus = [0]\n[threads.bad]\npolicy = 'x'");
    const auto s = forge::rt::thread_settings(cfg, "rx");
    REQUIRE(s);
    CHECK(s->cpus == std::vector<int>{0});
    CHECK_FALSE(forge::rt::thread_settings(cfg, "none"));
    CHECK_THROWS_AS(forge::rt::thread_settings(cfg, "bad"), forge::ConfigError);
    std::string err;
    std::thread t([&] { err = forge::rt::apply(*s); });
    t.join();
    CHECK(err.empty());
    forge::rt::ThreadSettings fifo;
    fifo.policy = "fifo";
    fifo.priority = 50;
    std::thread t2([&] { err = forge::rt::apply(fifo); });  // may be refused without CAP_SYS_NICE, never throws
    t2.join();
    CHECK(forge::rt::describe_current_thread().find("policy") == 0);
}

namespace {
// Minimal telnet client: send a line, read until `until` shows up.
struct Telnet {
    int fd = -1;
    explicit Telnet(int port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<std::uint16_t>(port));
        ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    }
    ~Telnet() { ::close(fd); }
    std::string read_until(const std::string& until, std::chrono::milliseconds timeout = 5s) {
        std::string got;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (got.find(until) == std::string::npos && std::chrono::steady_clock::now() < deadline) {
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) continue;
            char buf[1024];
            const auto n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            got.append(buf, static_cast<std::size_t>(n));
        }
        return got;
    }
    void line(const std::string& text) {
        const auto s = text + "\r\n";
        (void)!::send(fd, s.data(), s.size(), 0);
    }
};
}  // namespace

TEST_CASE("telnet: remote console sessions with login, roles, commands and live logs") {
    forge::testing::AppRunner<RolesApp> r({"-D", "console.telnet=\"127.0.0.1:0\"", "-D", "auth.enabled=true",
                                           "-D", "auth.users.otto.password=\"pw\"", "-D",
                                           "auth.users.otto.role=\"operator\"", "-D", "auth.users.vera.password=\"pv\"",
                                           "-D", "auth.users.vera.role=\"viewer\""});
    const auto ep = r->endpoints().at("telnet");
    const int port = std::stoi(ep.substr(ep.rfind(':') + 1));
    {
        Telnet t(port);
        CHECK(t.read_until("login: ").find("rolesapp 1.0") != std::string::npos);
        t.line("otto");
        CHECK(t.read_until("password: ").find("password") != std::string::npos);
        t.line("wrong");
        CHECK(t.read_until("login: ").find("login incorrect") != std::string::npos);
        t.line("otto");
        t.read_until("password: ");
        t.line("pw");
        CHECK(t.read_until("rolesapp> ").find("signed in as otto (operator)") != std::string::npos);
        t.line("set gain 42");
        CHECK(t.read_until("rolesapp> ").find("gain = 42") != std::string::npos);
        t.line("set power 3");  // admin only
        CHECK(t.read_until("rolesapp> ").find("needs role admin") != std::string::npos);
        t.line("whoami");
        CHECK(t.read_until("rolesapp> ").find("otto (operator)") != std::string::npos);
        t.line("logs on");
        t.read_until("rolesapp> ");
        r->logger()->info("hello from the application");
        CHECK(t.read_until("hello from the application").find("hello") != std::string::npos);
        t.line("logs off");
        t.read_until("rolesapp> ");
        t.line("call sweep points=3");
        CHECK(t.read_until("rolesapp> ").find("\"points\": 3") != std::string::npos);
        t.line("exit");
    }
    CHECK(r->gain == 42);
    {
        Telnet v(port);
        v.read_until("login: ");
        v.line("vera");
        v.read_until("password: ");
        v.line("pv");
        v.read_until("rolesapp> ");
        v.line("set gain 1");
        CHECK(v.read_until("rolesapp> ").find("needs role operator") != std::string::npos);
    }
    CHECK(r->gain == 42);
    const auto audit = r->audit().list();
    CHECK(std::any_of(audit.begin(), audit.end(), [](const v1::AuditRecord& a) {
        return a.channel() == "telnet" && a.user() == "otto" && a.target() == "gain";
    }));
}

TEST_CASE("logs: JSON format with trace id, history and listeners") {
    forge::testing::TempDir dir;
    const auto file = (dir / "app.log").string();
    forge::log::Options o;
    o.name = "jsonlog";
    o.console = false;
    o.file = file;
    o.format = "json";
    forge::log::init(o);
    std::vector<std::string> seen;
    const int id = forge::log::add_listener([&](const v1::LogRecord& r) { seen.push_back(r.message()); });
    const auto ctx = forge::trace::new_trace();
    {
        forge::trace::ContextScope scope(ctx);
        forge::log::get()->info("structured {}", 42);
    }
    forge::log::remove_listener(id);
    forge::log::flush();
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    const auto j = json::parse(line);
    CHECK(j["message"] == "structured 42");
    CHECK(j["level"] == "info");
    CHECK(j["logger"] == "jsonlog");
    CHECK(j["trace"] == ctx.trace_hex());
    CHECK(seen == std::vector<std::string>{"structured 42"});
    CHECK(forge::log::history(1).back().message() == "structured 42");
    forge::log::Options plain;
    plain.console = false;
    forge::log::init(plain);
}

TEST_CASE("crash: a fatal signal leaves a report with a backtrace") {
    forge::testing::TempDir dir;
    const pid_t pid = ::fork();
    if (pid == 0) {
        forge::crash::install("crashy", dir.path().string());
        const char line[] = "last words before the crash\n";
        forge::crash::remember_log_line(line, sizeof(line) - 1);
        ::raise(SIGSEGV);
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status));
    const auto report = forge::crash::previous_report("crashy", dir.path().string());
    REQUIRE(report);
    std::ifstream in(*report);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("SIGSEGV") != std::string::npos);
    CHECK(text.find("backtrace:") != std::string::npos);
    CHECK(text.find("last words before the crash") != std::string::npos);
}

namespace {
struct InvokeApp : forge::Application {
    InvokeApp() : Application({"invoke-app", "1.0.0"}) {}
    int value = 5;
};
}  // namespace

TEST_CASE("framework: invoke() runs on the main loop and carries errors back as new exceptions") {
    forge::testing::AppRunner<InvokeApp> r;
    CHECK_FALSE(r->on_main_loop());
    CHECK(r->invoke([&] { return r->on_main_loop(); }));
    int& ref = r->invoke([&]() -> int& { return r->value; });
    CHECK(&ref == &r->value);
    r->invoke([] {});
    try {
        r->invoke([]() -> int { throw forge::http::HttpError(409, "busy", forge::json{{"holder", "x"}}); });
        FAIL("expected HttpError");
    } catch (const forge::http::HttpError& e) {
        CHECK(e.status() == 409);
        CHECK(std::string(e.what()) == "busy");
        CHECK(e.body()["holder"] == "x");
    }
    CHECK_THROWS_AS(r->invoke([] { throw forge::ConfigError("bad key"); }), forge::ConfigError);
    CHECK_THROWS_AS(r->invoke([] { throw std::out_of_range("x"); }), std::out_of_range);
    CHECK_THROWS_WITH_AS(r->invoke([] { throw 42; }), "unknown exception", std::runtime_error);
}

TEST_CASE("framework: a blocked main loop does not hang requests (invoke_for, 503 main loop busy)") {
    forge::testing::AppRunner<RolesApp> r({"-D", "http.enabled=true", "-D", "http.port=0", "-D",
                                           "http.host=\"127.0.0.1\"", "-D", "forge.request_timeout_ms=150"});
    CHECK(r->request_timeout() == 150ms);
    httplib::Client http("127.0.0.1", r->http()->port());
    http.set_read_timeout(5, 0);

    // Not busy: answers as usual.
    CHECK(r->invoke_for(50ms, [] { return 7; }) == 7);
    CHECK(http.Get("/forge/api/state")->status == 200);

    std::mutex m;
    std::condition_variable cv;
    bool release = false, blocked = false;
    r->post([&] {
        std::unique_lock lock(m);
        blocked = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    });
    {
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return blocked; });
    }

    const auto t0 = std::chrono::steady_clock::now();
    auto res = http.Get("/forge/api/state");
    REQUIRE(res);
    CHECK(res->status == 503);
    CHECK(res->body.find("main loop busy") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - t0 < 2s);
    CHECK(http.Get("/forge/api/controls/gain")->status == 503);
    res = http.Post("/forge/api/controls/gain", R"({"value": 50})", "application/json");
    CHECK(res->status == 503);
    res = http.Post("/forge/api/rpc", R"({"get":{"id":"gain"}})", "application/json");
    REQUIRE(res);
    const auto rpc = json::parse(res->body);
    CHECK_FALSE(rpc.value("ok", false));
    CHECK(rpc["status"] == 503);
    CHECK_THROWS_AS(r->invoke_for(20ms, [] { return 1; }), forge::Error);
    // What does not need the main loop still answers.
    CHECK(http.Get("/health")->status == 200);
    CHECK(http.Get("/metrics")->body.find("forge_control_timeouts ") != std::string::npos);

    {
        std::lock_guard lock(m);
        release = true;
    }
    cv.notify_all();
    // The request that timed out was not applied later.
    CHECK(r->invoke([&] { return r->gain; }) == 10);
    CHECK(http.Get("/forge/api/state")->status == 200);
}

TEST_CASE("web: card layouts are stored on the device and survive a restart") {
    forge::testing::TempDir dir;
    const auto file = (dir / "state" / "web-layout.json").string();
    const json layout{{"version", 1},
                      {"items", json::array({{{"id", "frequency"}, {"x", 0}, {"y", 0}, {"w", 6}, {"h", 30}, {"fit", false}},
                                             {{"id", "markers"}, {"x", 6}, {"y", 0}, {"w", 6}, {"h", 50}, {"fit", true}}})}};
    const std::vector<std::string> args{"-D", "http.enabled=true", "-D", "http.port=0", "-D", "http.host=\"127.0.0.1\"",
                                        "-D", "web.layout_file=" + file};
    auto stored = [](const httplib::Result& res) {  // the layout without the "scope" it was read from
        json j = json::parse(res->body);
        CHECK(j["scope"] == "device");
        j.erase("scope");
        return j;
    };
    {
        forge::testing::AppRunner<RolesApp> r(args);
        httplib::Client http("127.0.0.1", r->http()->port());
        CHECK(http.Get("/forge/api/layout/panel")->status == 204);  // nothing stored yet
        auto res = http.Put("/forge/api/layout/panel", layout.dump(), "application/json");
        REQUIRE(res);
        CHECK(res->status == 200);
        CHECK(json::parse(res->body)["saved"] == true);
        CHECK(stored(http.Get("/forge/api/layout/panel")) == layout);
        // invalid page names and bodies are refused
        CHECK(http.Put("/forge/api/layout/..%2Fx", layout.dump(), "application/json")->status >= 400);
        CHECK(http.Put("/forge/api/layout/a%20b", layout.dump(), "application/json")->status == 400);
        CHECK(http.Put("/forge/api/layout/panel", "[1,2]", "application/json")->status == 400);
        CHECK(http.Put("/forge/api/layout/hub", R"({"items": 5})", "application/json")->status == 400);
        http.Put("/forge/api/layout/other", layout.dump(), "application/json");
        CHECK(http.Delete("/forge/api/layout/other")->status == 204);
    }
    REQUIRE(std::filesystem::exists(file));
    {
        // Restarted: the layout is still there; the deleted one is not.
        forge::testing::AppRunner<RolesApp> r(args);
        httplib::Client http("127.0.0.1", r->http()->port());
        CHECK(stored(http.Get("/forge/api/layout/panel")) == layout);
        CHECK(http.Get("/forge/api/layout/other")->status == 204);
    }
    {
        // "-": not stored on the device (the page falls back to the browser's storage).
        forge::testing::AppRunner<RolesApp> r({"-D", "http.enabled=true", "-D", "http.port=0", "-D",
                                               "http.host=\"127.0.0.1\"", "-D", "web.layout_file=-"});
        httplib::Client http("127.0.0.1", r->http()->port());
        auto res = http.Put("/forge/api/layout/panel", layout.dump(), "application/json");
        CHECK(json::parse(res->body)["saved"] == false);
    }
    // The vendored GridStack is served with the panel.
    forge::testing::AppRunner<RolesApp> r({"-D", "http.enabled=true", "-D", "http.port=0", "-D", "http.host=\"127.0.0.1\""});
    httplib::Client http("127.0.0.1", r->http()->port());
    const auto js = http.Get("/forge/vendor/gridstack/gridstack-all.js");
    REQUIRE(js);
    CHECK(js->status == 200);
    CHECK(js->body.find("GridStack") != std::string::npos);
    CHECK(http.Get("/forge/vendor/gridstack/gridstack.min.css")->status == 200);
}

TEST_CASE("web: signed-in users keep their own card layouts; the device's is the default") {
    forge::testing::TempDir dir;
    const std::vector<std::string> args{
        "-D", "http.enabled=true", "-D", "http.port=0", "-D", "http.host=\"127.0.0.1\"",
        "-D", "web.layout_file=" + (dir / "layout.json").string(), "-D", "auth.enabled=true",
        "-D", "auth.users.vera.token=\"v\"", "-D", "auth.users.vera.role=\"viewer\"",
        "-D", "auth.users.ada.token=\"a\"", "-D", "auth.users.ada.role=\"admin\""};
    const json device{{"items", json::array({{{"id", "a"}, {"x", 0}, {"y", 0}, {"w", 12}, {"h", 10}}})}};
    const json mine{{"items", json::array({{{"id", "a"}, {"x", 6}, {"y", 0}, {"w", 6}, {"h", 20}}})}};
    forge::testing::AppRunner<RolesApp> r(args);
    httplib::Client http("127.0.0.1", r->http()->port());
    const httplib::Headers vera{{"Authorization", "Bearer v"}}, ada{{"Authorization", "Bearer a"}};
    auto get = [&](const httplib::Headers& h, const std::string& q = "") {
        auto res = http.Get("/forge/api/layout/panel" + q, h);
        return res->status == 200 ? json::parse(res->body) : json(nullptr);
    };

    // The device's layout: an admin with ?scope=device; anonymous viewers may not.
    CHECK(http.Put("/forge/api/layout/panel?scope=device", device.dump(), "application/json")->status == 403);
    auto res = http.Put("/forge/api/layout/panel?scope=device", ada, device.dump(), "application/json");
    CHECK(json::parse(res->body)["scope"] == "device");
    CHECK(get(vera)["scope"] == "device");  // nothing of her own yet
    CHECK(get(vera)["items"] == device["items"]);

    // Vera arranges the page for herself; others keep the device's.
    res = http.Put("/forge/api/layout/panel", vera, mine.dump(), "application/json");
    CHECK(res->status == 200);
    CHECK(json::parse(res->body)["scope"] == "user");
    CHECK(get(vera)["scope"] == "user");
    CHECK(get(vera)["items"] == mine["items"]);
    CHECK(get(ada)["items"] == device["items"]);
    CHECK(get({})["items"] == device["items"]);
    CHECK(get(vera, "?scope=device")["items"] == device["items"]);
    // Reset: back to the device's.
    CHECK(http.Delete("/forge/api/layout/panel", vera)->status == 204);
    CHECK(get(vera)["scope"] == "device");
    CHECK(get(ada)["items"] == device["items"]);
}

namespace {
// Timer-driven logic: a tick counter and an alarm that needs 3 s of overheating
// (hysteresis) before it is raised and 2 s of normal readings to clear.
struct TimedApp : forge::Application {
    TimedApp() : Application({"timed-app", "1.0"}) {}
    int ticks = 0;
    double temperature = 20;
    std::vector<double> tick_times;  // now() seen by each tick, s since start
    forge::Scheduler::Clock::time_point start, hot_since{}, cool_since{};

protected:
    void on_start() override {
        start = now();
        every(std::chrono::seconds(1), [this] {
            ++ticks;
            tick_times.push_back(std::chrono::duration<double>(now() - start).count());
            const bool hot = temperature > 80;
            if (hot) {
                cool_since = {};
                if (hot_since == forge::Scheduler::Clock::time_point{}) hot_since = now();
                if (now() - hot_since >= std::chrono::seconds(3)) alarms().raise("hot", forge::Severity::warning, "too hot");
            } else {
                hot_since = {};
                if (cool_since == forge::Scheduler::Clock::time_point{}) cool_since = now();
                if (now() - cool_since >= std::chrono::seconds(2)) alarms().clear("hot");
            }
        });
    }
};
}  // namespace

TEST_CASE("testing: manual time - timers run when the test advances the clock, without sleeping") {
    const auto wall = std::chrono::steady_clock::now();
    forge::testing::AppRunner<TimedApp> r({}, {.manual_time = true});
    CHECK(r->manual_clock());
    CHECK(r.advance(std::chrono::milliseconds(500)) >= 0);
    CHECK(r->invoke([&] { return r->ticks; }) == 0);
    r.advance(std::chrono::seconds(10));
    CHECK(r->invoke([&] { return r->ticks; }) == 10);
    // every tick saw the time it was due at
    CHECK(r->invoke([&] { return r->tick_times; }) == std::vector<double>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10});

    // hysteresis: hot for 2 s is not enough, 3 s is
    r->invoke([&] { r->temperature = 95; });
    r.advance(std::chrono::seconds(3));  // ticks at 11, 12, 13: hot since 11 -> 2 s
    CHECK_FALSE(r->alarms().active("hot"));
    r.advance(std::chrono::seconds(1));  // 14: 3 s
    CHECK(r->alarms().active("hot"));
    r->invoke([&] { r->temperature = 20; });
    r.advance(std::chrono::seconds(2));  // cool since 15, at 16: 1 s
    CHECK(r->alarms().active("hot"));
    r.advance(std::chrono::seconds(1));
    CHECK_FALSE(r->alarms().active("hot"));

    // A day of timers in a moment.
    r.advance(std::chrono::hours(24));
    CHECK(r->invoke([&] { return r->ticks; }) == 17 + 86400);
    CHECK(std::chrono::steady_clock::now() - wall < std::chrono::seconds(30));

    // Real clock: advance_time is refused.
    forge::testing::AppRunner<TimedApp> real;
    CHECK_THROWS_AS(real.advance(std::chrono::seconds(1)), std::logic_error);
}

TEST_CASE("instances: --instance names the application <name>@<instance>") {
    {
        forge::testing::AppRunner<InvokeApp> r({"--instance", "rx2"});
        CHECK(r->instance() == "rx2");
        CHECK(r->id() == "invoke-app@rx2");
    }
    forge::testing::AppRunner<InvokeApp> plain;
    CHECK(plain->instance().empty());
    CHECK(plain->id() == "invoke-app");
}

TEST_CASE("daemon naming standard: lowercase words separated by '-'") {
    for (const char* ok : {"radio-controller", "forge-hub", "pipeline", "sensor-service", "app2", "a-b-c"}) {
        CHECK_MESSAGE(forge::is_standard_name(ok), ok);
    }
    for (const char* bad : {"sensor_service", "RadioController", "radio--controller", "-radio", "radio-", "2app",
                            "radio controller", ""}) {
        CHECK_FALSE_MESSAGE(forge::is_standard_name(bad), bad);
    }
}
