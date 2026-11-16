// The telemetry-store example: records wait while there is no contact, go
// out compressed during a pass, and survive a restart.
#include <forge/forge.hpp>
#include <forge/testing.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>

// main.cpp ends with FORGE_MAIN(TelemetryStore); the test has its own main().
#undef FORGE_MAIN
#define FORGE_MAIN(AppClass)
#include "main.cpp"

namespace {

using Runner = forge::testing::AppRunner<TelemetryStore>;

std::vector<std::string> args(const forge::testing::TempDir& dir) {
    return {"-D", "http.enabled=false",
            "-D", "telemetry.backlog=" + (dir / "backlog.db").string(),
            "-D", "telemetry.outbox=" + (dir / "outbox").string(),
            "-D", "telemetry.downlink_bps=0",
            "-D", "telemetry.batch_bytes=4096"};
}

std::size_t waiting(Runner& r) { return r->backlog().size(); }

}  // namespace

TEST_CASE("telemetry-store: records wait without contact and go out compressed during a pass") {
    forge::testing::TempDir dir;
    Runner r(args(dir), {.manual_time = true});
    r.advance(10s);  // 100 records, no contact
    CHECK(waiting(r) == 100);
    CHECK(std::filesystem::is_empty(dir / "outbox"));

    r->invoke([&] { r->dashboard()->set("contact", true); });  // the pass begins
    REQUIRE(forge::testing::wait_until([&] { return waiting(r) == 0; }));
    r->invoke([&] { r->dashboard()->set("contact", false); });

    // Every record is in the outbox, once, in order, in LZ4 files.
    std::vector<std::string> lines;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir / "outbox")) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    CHECK(files.size() > 1);  // 4 kB batches
    std::size_t packed = 0, raw = 0;
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
        std::vector<std::uint8_t> text;
        REQUIRE(forge::lz4::decompress(bytes, text).ok());
        packed += bytes.size();
        raw += text.size();
        std::string all(text.begin(), text.end());
        for (std::size_t at = 0; at < all.size();) {
            const auto end = all.find('\n', at);
            lines.push_back(all.substr(at, end - at));
            at = end + 1;
        }
    }
    REQUIRE(lines.size() == 100);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        CHECK(forge::json::parse(lines[i])["seq"] == i + 1);
    }
    CHECK(raw > 2 * packed);  // housekeeping repeats itself
    CHECK(r->invoke([&] { return r->dashboard()->get("records_sent"); }) == 100);
}

TEST_CASE("telemetry-store: the backlog is still there after a restart") {
    forge::testing::TempDir dir;
    {
        Runner r(args(dir), {.manual_time = true});
        r.advance(5s);
        CHECK(waiting(r) == 50);
    }
    Runner r(args(dir), {.manual_time = true});
    CHECK(waiting(r) == 50);
    r.advance(1s);
    CHECK(waiting(r) == 60);
}

TEST_CASE("telemetry-store: when the backlog is full the oldest records go") {
    forge::testing::TempDir dir;
    auto a = args(dir);
    a.insert(a.end(), {"-D", "telemetry.max_records=20"});
    Runner r(a, {.manual_time = true});
    r.advance(5s);
    CHECK(waiting(r) == 20);
    CHECK(r->backlog().dropped() == 30);
    const auto oldest = r->backlog().peek(1);
    REQUIRE(oldest.size() == 1);
    const auto first = forge::json::parse(std::string(oldest[0].data.begin(), oldest[0].data.end()));
    CHECK(first["seq"] == 31);
}

TEST_CASE("telemetry-store: every pass is in the pass log (SQL, forge::sqlite)") {
    forge::testing::TempDir dir;
    {
        Runner r(args(dir), {.manual_time = true});
        r.advance(5s);  // 50 records wait
        r->invoke([&] { r->dashboard()->set("contact", true); });
        REQUIRE(forge::testing::wait_until([&] { return waiting(r) == 0; }));
        r->invoke([&] { r->dashboard()->set("contact", false); });
        CHECK(r->invoke([&] { return r->dashboard()->get("passes"); }) == 1);
        CHECK(r->invoke([&] { return r->dashboard()->get("last_pass"); })
                  .get<std::string>()
                  .starts_with("#1: 50 records"));

        r->invoke([&] { r->dashboard()->set("contact", true); });  // a second pass, open when the program stops
    }

    // The same file, read by anyone with SQLite: the program closed the open pass when it stopped.
    forge::sqlite::Database db((dir / "backlog.db").string(), {.read_only = true});
    auto q = db.prepare("SELECT id, start_ms, end_ms, records, packed_bytes FROM passes ORDER BY id");
    REQUIRE(q.step());
    CHECK(q.integer(0) == 1);
    CHECK(q.integer(2) >= q.integer(1));
    CHECK(q.integer(3) == 50);
    CHECK(q.integer(4) > 0);
    REQUIRE(q.step());
    CHECK(q.integer(0) == 2);
    CHECK_FALSE(q.null(2));
    CHECK(q.integer(3) == 0);
    CHECK_FALSE(q.step());
}

TEST_CASE("telemetry-store: the console lists the passes") {
    forge::testing::TempDir dir;
    Runner r(args(dir), {.manual_time = true});
    for (int i = 0; i < 3; ++i) {
        r->invoke([&] { r->dashboard()->set("contact", true); });
        r->invoke([&] { r->dashboard()->set("contact", false); });
    }
    auto run = [&](const std::string& line) {
        std::string out;
        forge::Console::Session session{.channel = "test", .output = [&](std::string_view s) { out += s; }};
        r->invoke([&] { CHECK(r->console()->execute(line, session)); });
        return out;
    };
    REQUIRE(r->console() != nullptr);
    const auto lines = [](const std::string& s) { return std::count(s.begin(), s.end(), '\n'); };
    CHECK(lines(run("passes")) == 4);    // the heading and three passes
    CHECK(lines(run("passes 1")) == 2);  // the heading and the newest
    CHECK(run("passes 1").find("   3  ") != std::string::npos);
}
